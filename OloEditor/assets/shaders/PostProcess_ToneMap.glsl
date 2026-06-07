#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture;
// Scene depth — used by the underwater fog stage to reconstruct eye-space
// distance for the Beer-Lambert falloff. Bound by ToneMapRenderPass.
layout(binding = 19) uniform sampler2D u_DepthTexture;
// Nearest wavy water-surface depth captured by WaterRenderPass (1.0 = no water
// at this pixel). Lets the fog find the real per-pixel water boundary instead
// of assuming a flat plane. Bound by ToneMapRenderPass (TEX_UNDERWATER_WATER_DEPTH).
layout(binding = 32) uniform sampler2D u_WaterSurfaceDepth;

#define TONEMAP_NONE      0
#define TONEMAP_REINHARD  1
#define TONEMAP_ACES      2
#define TONEMAP_UNCHARTED2 3

// Automatic-exposure result written by AutoExposureAverage.comp.
//   autoExposureState[0] = exposure multiplier; <= 0 means auto-exposure is off
//   this frame, so the manual u_Exposure from the PostProcess UBO is used.
layout(std430, binding = 20) readonly buffer ExposureState
{
    float autoExposureState[];
};

layout(std140, binding = 7) uniform PostProcessUBO
{
    int   u_TonemapOperator;
    float u_Exposure;
    float u_Gamma;
    float u_BloomThreshold;

    float u_BloomIntensity;
    float u_VignetteIntensity;
    float u_VignetteSmoothness;
    float u_ChromaticAberrationIntensity;

    float u_DOFFocusDistance;
    float u_DOFFocusRange;
    float u_DOFBokehRadius;
    float u_MotionBlurStrength;

    int   u_MotionBlurSamples;
    float u_InverseScreenWidth;
    float u_InverseScreenHeight;
    float _padding0;

    float u_TexelSizeX;
    float u_TexelSizeY;
    float u_Near;
    float u_Far;
};

// Underwater fog UBO (binding 36) — mirrors UnderwaterFogUBOData. Drives a
// per-pixel water-volume fog: each pixel's view ray is fogged by the length of
// its segment that passes below the water plane, so the waterline is handled
// per pixel (underwater half fogged, above-water half clear) rather than as a
// whole-screen toggle. Flags.x < 0.5 disables it. See WATER_FUTURE_IMPROVEMENTS.md §7.2.
layout(std140, binding = 37) uniform UnderwaterFogBlock
{
    vec4 u_UnderwaterColorAndDensity; // rgb = fog colour, a = per-metre density
    vec4 u_UnderwaterFlags;           // x = active (>0.5), y = waterSurfaceY, z = time, w = pad
    vec4 u_UnderwaterCameraPos;       // xyz = camera world position, w = pad
    vec4 u_UnderwaterRefraction;      // x = strength (UV units, 0 = off), y = scale, z = speed, w = chromatic
    vec4 u_UnderwaterCaustics;        // x = intensity (0 = off), y = scale, z = speed, w = maxDepth
    vec4 u_UnderwaterCausticColor;    // rgb = caustic tint, w = sun-overhead factor max(-sunDir.y, 0)
    mat4 u_UnderwaterInvViewProj;     // NDC -> world reconstruction
};

// Animated screen-space UV offset for the submerged refraction wobble.
// MUST match UnderwaterCaustics::RefractionOffset (UnderwaterCaustics.h).
vec2 underwaterRefractionOffset(vec2 uv, float time, float strength, float scale, float speed)
{
    if (strength <= 0.0)
        return vec2(0.0);
    float s = clamp(strength, 0.0, 0.1);
    float phase = time * speed;
    float ox = sin(uv.y * scale + phase);
    float oy = cos(uv.x * scale + phase * 0.9 + 1.3);
    return vec2(ox, oy) * s;
}

// One octave of the caustic web — a thin wavy ridge along the zero-contour of a
// pair of drifting sines. MUST match UnderwaterCaustics::CausticOctave.
float underwaterCausticOctave(float x, float y, float t)
{
    float s = (sin(x + t) + sin(y * 1.2 - t * 0.8)) * 0.5;
    float r = max(1.0 - abs(s), 0.0);
    return r * r * r;
}

// Procedural caustic pattern in [0,1] from a world-space XZ point — two octaves
// of wavy ridge lines unioned into a caustic web. MUST match
// UnderwaterCaustics::CausticPattern.
float underwaterCausticPattern(vec2 worldXZ, float time, float scale, float speed)
{
    vec2 q = worldXZ * scale;
    float t = time * speed;
    float o1 = underwaterCausticOctave(q.x, q.y, t);
    float o2 = underwaterCausticOctave(q.x * 1.7 + 5.0, q.y * 1.7 - 3.0, -t * 1.1);
    return max(o1, o2);
}

// Depth fade for caustics. MUST match UnderwaterCaustics::CausticDepthFade.
float underwaterCausticDepthFade(float depthBelowSurface, float maxDepth)
{
    if (depthBelowSurface <= 0.0 || maxDepth <= 0.0)
        return 0.0;
    return clamp(1.0 - depthBelowSurface / maxDepth, 0.0, 1.0);
}

// Length of the part of segment [camPos -> worldPos] below the water plane.
// MUST match UnderwaterFog::UnderwaterSegmentLength (UnderwaterFog.h).
float underwaterSegmentLength(vec3 camPos, vec3 worldPos, float waterY)
{
    bool camUnder = camPos.y < waterY;
    bool fragUnder = worldPos.y < waterY;
    if (camUnder && fragUnder)
        return length(worldPos - camPos);
    if (!camUnder && !fragUnder)
        return 0.0;
    float dy = worldPos.y - camPos.y;
    if (abs(dy) < 1e-6)
        return 0.0;
    float tCross = (waterY - camPos.y) / dy;
    vec3 crossPoint = camPos + (worldPos - camPos) * tCross;
    return camUnder ? length(crossPoint - camPos) : length(worldPos - crossPoint);
}

// Underwater shading for this pixel: adds animated caustics to the submerged
// surface radiance (§7.1) and then applies Beer-Lambert absorption over the
// underwater segment of the view ray (§7.2). The fog blend mirrors UnderwaterFog
// (UnderwaterFog.h) and the caustic math mirrors UnderwaterCaustics
// (UnderwaterCaustics.h); both are pinned by the WaterRendering tests.
vec3 applyUnderwaterFog(vec3 color, vec2 uv)
{
    if (u_UnderwaterFlags.x < 0.5)
        return color;

    float depth = texture(u_DepthTexture, uv).r;
    // Reconstruct the world position of the opaque geometry (or far plane) at
    // this pixel; the segment between the camera and it that lies underwater is
    // what absorbs light.
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldH = u_UnderwaterInvViewProj * ndc;
    vec3 worldPos = worldH.xyz / worldH.w;

    // Per-pixel water surface height: where the water pass captured a surface in
    // front of this pixel, reconstruct its world Y (wave-accurate) and use it as
    // the local water plane. Otherwise fall back to the flat surface Y. This is
    // what lets the fog follow the waves and work from any camera height — the
    // old flat-plane assumption couldn't tell the surface from the seafloor.
    float surfaceY = u_UnderwaterFlags.y;
    float wDepth = texture(u_WaterSurfaceDepth, uv).r;
    if (wDepth < 1.0)
    {
        vec4 sNdc = vec4(uv * 2.0 - 1.0, wDepth * 2.0 - 1.0, 1.0);
        vec4 sH = u_UnderwaterInvViewProj * sNdc;
        surfaceY = (sH.xyz / sH.w).y;
    }

    // Caustics (§7.1): animated light projected onto submerged geometry. Added to
    // the surface radiance BEFORE the Beer-Lambert absorption below, so distant
    // caustics fade into the fog exactly like the rest of the seabed's light.
    // Derivatives are taken in uniform control flow (this whole function is gated
    // by the UBO-uniform Active flag), then the per-pixel terms branch.
    vec3 dPdx = dFdx(worldPos);
    vec3 dPdy = dFdy(worldPos);
    vec3 nCross = cross(dPdx, dPdy);
    vec3 geoN = (dot(nCross, nCross) > 1e-12) ? normalize(nCross) : vec3(0.0, 1.0, 0.0);
    if (depth < 0.9999 && u_UnderwaterCaustics.x > 0.0)
    {
        float upFacing = max(geoN.y, 0.0);
        float fade = underwaterCausticDepthFade(surfaceY - worldPos.y, u_UnderwaterCaustics.w);
        float pattern = underwaterCausticPattern(worldPos.xz, u_UnderwaterFlags.z,
                                                 u_UnderwaterCaustics.y, u_UnderwaterCaustics.z);
        float caustic = pattern * fade * upFacing * u_UnderwaterCausticColor.w * u_UnderwaterCaustics.x;
        color += u_UnderwaterCausticColor.rgb * caustic;
    }

    float underwaterDist = underwaterSegmentLength(u_UnderwaterCameraPos.xyz, worldPos, surfaceY);

    float density = clamp(u_UnderwaterColorAndDensity.a, 0.0, 10.0);
    float transmittance = exp(-density * underwaterDist);
    vec3 fogColor = u_UnderwaterColorAndDensity.rgb;
    return color * transmittance + fogColor * (1.0 - transmittance);
}

vec3 reinhardToneMapping(vec3 color)
{
    return color / (color + vec3(1.0));
}

vec3 acesToneMapping(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 uncharted2ToneMapping(vec3 color)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

void main()
{
    // Submerged refraction distortion (§7.2, bullet 2): when the camera is below
    // the surface, wobble the scene-colour sample UV and split RGB for a cheap
    // chromatic refraction. Gated on the underwater flag so above-water rendering
    // is bit-for-bit unchanged. The depth/world reconstruction in
    // applyUnderwaterFog still uses the TRUE pixel UV (v_TexCoord) so fog distance
    // and caustic projection stay geometrically correct.
    vec3 hdrColor;
    if (u_UnderwaterFlags.x >= 0.5 && u_UnderwaterRefraction.x > 0.0)
    {
        vec2 off = underwaterRefractionOffset(v_TexCoord, u_UnderwaterFlags.z,
                                              u_UnderwaterRefraction.x,
                                              u_UnderwaterRefraction.y,
                                              u_UnderwaterRefraction.z);
        vec2 baseUV = clamp(v_TexCoord + off, 0.0, 1.0);
        float chroma = u_UnderwaterRefraction.w;
        if (chroma > 0.0)
        {
            vec2 cOff = off * chroma;
            hdrColor.r = texture(u_Texture, clamp(baseUV + cOff, 0.0, 1.0)).r;
            hdrColor.g = texture(u_Texture, baseUV).g;
            hdrColor.b = texture(u_Texture, clamp(baseUV - cOff, 0.0, 1.0)).b;
        }
        else
        {
            hdrColor = texture(u_Texture, baseUV).rgb;
        }
    }
    else
    {
        hdrColor = texture(u_Texture, v_TexCoord).rgb;
    }

    // Underwater absorption + caustics — applied in linear HDR radiance before
    // exposure so the in-scatter colour is exposed/tonemapped with the rest of
    // the scene.
    hdrColor = applyUnderwaterFog(hdrColor, v_TexCoord);

    // Apply exposure. Auto-exposure (when enabled) writes a positive metered
    // multiplier into autoExposureState[0]. A non-positive sentinel — or an
    // out-of-range value from an unbound buffer when a test/tool drives this
    // shader without the metering pass — falls back to the manual u_Exposure.
    // The finite upper bound keeps a stray read from overflowing the tone-map
    // operators on extreme HDR input.
    float autoExp = autoExposureState[0];
    float exposure = (autoExp > 0.0 && autoExp < 1.0e6) ? autoExp : u_Exposure;
    hdrColor *= exposure;

    // Tone mapping
    vec3 mapped;
    switch (u_TonemapOperator)
    {
        case TONEMAP_REINHARD:
            mapped = reinhardToneMapping(hdrColor);
            break;
        case TONEMAP_ACES:
            mapped = acesToneMapping(hdrColor);
            break;
        case TONEMAP_UNCHARTED2:
            mapped = uncharted2ToneMapping(hdrColor);
            break;
        default:
            mapped = clamp(hdrColor, 0.0, 1.0);
            break;
    }

    // Gamma correction
    mapped = pow(mapped, vec3(1.0 / u_Gamma));

    o_Color = vec4(mapped, 1.0);
}
