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
    vec4 u_UnderwaterFlags;           // x = active (>0.5), y = waterSurfaceY, zw = pad
    vec4 u_UnderwaterCameraPos;       // xyz = camera world position, w = pad
    mat4 u_UnderwaterInvViewProj;     // NDC -> world reconstruction
};

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

// Beer-Lambert absorption over the underwater segment of this pixel's view ray.
// Mirrors UnderwaterFog (UnderwaterFog.h), pinned by the WaterRendering tests.
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
    vec3 hdrColor = texture(u_Texture, v_TexCoord).rgb;

    // Underwater absorption — applied in linear HDR radiance before exposure
    // so the in-scatter colour is exposed/tonemapped with the rest of the scene.
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
