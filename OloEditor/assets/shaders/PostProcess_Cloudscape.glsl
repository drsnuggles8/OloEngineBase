// =============================================================================
// PostProcess_Cloudscape.glsl — volumetric cloud raymarch (issue #633, pass A)
//
// Half-resolution fullscreen pass: raymarches the two-layer Perlin-Worley
// cloud field (CloudscapeCommon.glsl) with cone-sampled sun/moon in-scatter
// (dual-lobe HG phase + powder + cheap multi-scatter octave), Beer-Lambert
// transmittance, blue-noise-style jittered start (interleaved gradient noise
// + frame index — the engine's established jitter idiom), and depth-aware
// termination against scene geometry.
//
// Output (RGBA16F): rgb = premultiplied in-scattered radiance,
//                   a   = transmittance along the view ray (1 = no cloud).
// Temporal accumulation happens in pass B (PostProcess_CloudscapeResolve).
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// binding 57 is the engine-wide vertex-pull binding; the root struct carries
// this buffer's device address, so the SAME 20-byte {vec3 position, vec2 uv}
// stream the attribute path consumes is read by index instead. OLO_VULKAN is
// defined only on the Vulkan shaderc route; the GL branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec3 position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
    v_TexCoord = vec2(b_Vertices.v[base + 3], b_Vertices.v[base + 4]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Cloud;

#include "include/NoiseCommon.glsl"
#include "include/CloudscapeCommon.glsl"
// Volumetric shadow map (issue #723): the cloud cascade carries the optical
// depth through the WHOLE layer, which the cone march below structurally
// cannot — see cloudLightNearRange().
#include "include/VolumetricShadowCommon.glsl"

// Full-res scene depth (TEX_POSTPROCESS_DEPTH).
#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DepthTexture OLO_HEAP_TEX_2D(19)  // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 19) uniform sampler2D u_DepthTexture;
#endif

// Shared 288-byte camera UBO (binding 0) — includes the render origin for
// camera-relative rendering (#429): cloud math runs in ABSOLUTE world space.
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

// Inverse VP for world-position reconstruction (MotionBlur UBO, binding 8).
layout(std140, binding = 8) uniform MotionBlurUBO {
    mat4 u_InverseViewProjection;
    mat4 u_MB_PrevViewProjection;
};

// Interleaved gradient noise (mirrors FogCommon.glsl / FroxelFogScatter.comp).
float cloudIGN(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// The NEAR range of the light march, in metres. Historically this was also the
// WHOLE march: capped at 1400 m, it could not reach the top of a 2500 m layer,
// so density above that height darkened nothing — the deep interior of a thick
// deck was lit exactly like its top, and no coverage setting could produce the
// "bright on the sun side, dark underneath" cue. The volumetric shadow map
// (issue #723) carries everything past this range; the cone keeps the near
// field, where its high-frequency taps beat a ~94 m/texel volume.
float cloudLightNearRange()
{
    return min(u_CloudLayer.y - u_CloudLayer.x, 1400.0);
}

// Optical depth toward the light over [0, maxT]: short cone of exponentially
// spaced cheap density taps (Schneider light march). Returns density-length —
// the caller applies kCloudExtinction.
float cloudLightOpticalDepth(vec3 worldPos, vec3 towardLight, int lightSteps, float maxT)
{
    float od = 0.0;
    float prevT = 0.0;
    for (int i = 1; i <= lightSteps; ++i)
    {
        float f = float(i) / float(lightSteps);
        float t = maxT * f * f; // exponential-ish spacing: dense near the sample
        vec3 p = worldPos + towardLight * t;
        od += cloudDensity(p, true) * (t - prevT);
        prevT = t;
    }
    return od;
}

void main()
{
    if (u_CloudMisc.w < 0.5)
    {
        o_Cloud = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }


    // Reconstruct the view ray in absolute world space.
    float depth = texture(u_DepthTexture, v_TexCoord).r;
    vec4 ndc = vec4(v_TexCoord * 2.0 - 1.0, 1.0, 1.0); // far plane for direction
    vec4 worldFar = u_InverseViewProjection * ndc;
    worldFar.xyz /= worldFar.w;
    vec3 cameraPos = u_CameraPosition + u_RenderOrigin;
    vec3 rayDir = normalize((worldFar.xyz + u_RenderOrigin) - cameraPos);

    // Geometry distance (absolute world) — clouds render behind geometry.
    float geomT = 1.0e12;
    if (depth < 0.9999)
    {
        vec4 geomNdc = vec4(v_TexCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec4 geomWorld = u_InverseViewProjection * geomNdc;
        geomWorld.xyz /= geomWorld.w;
        geomT = length((geomWorld.xyz + u_RenderOrigin) - cameraPos);
    }

    vec2 slab = cloudLayerIntersect(cameraPos, rayDir);
    const float kMaxMarchDistance = 40000.0;
    float tStart = slab.x;
    float tEnd = min(min(slab.y, geomT), kMaxMarchDistance);


    if (tEnd <= tStart + 1.0)
    {
        o_Cloud = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    int steps = int(u_CloudMap.y);
    int lightSteps = int(u_CloudMap.z);
    float stepLen = (tEnd - tStart) / float(steps);
    // Jittered start decorrelates banding; the frame-index offset rotates the
    // pattern so pass B's temporal blend integrates it away.
    float jitter = cloudIGN(gl_FragCoord.xy + fract(u_CloudMisc.y * 0.618) * vec2(5.13, 7.77));
    float t = tStart + stepLen * jitter;

    // sigma_t per unit density: kCloudExtinction from CloudscapeCommon —
    // shared with the shadow map so ground shadows match the clouds.
    const float kExtinction = kCloudExtinction;
    float cosTheta = dot(rayDir, u_CloudSunDir.xyz);
    float phase = cloudPhase(cosTheta, u_CloudMap.w);
    // Rain-laden clouds scatter less light (darker bases).
    float wetnessDarken = 1.0 - 0.5 * u_CloudField.w;

    vec3 inscatter = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < steps; ++i)
    {
        vec3 samplePos = cameraPos + rayDir * t;
        float density = cloudDensity(samplePos, false);
        if (density > 1.0e-4)
        {
            float extinction = density * kExtinction;
            float stepTrans = exp(-extinction * stepLen);

            // Sun/moon in-scatter with powder + one cheap multi-scatter octave
            // (reduced-extinction re-evaluation, Wrenninge-style).
            //
            // The light path splits at cloudLightNearRange(): the cone march
            // owns [0, near] and the volumetric shadow map owns [near, the
            // light]. They COMPOSE rather than overlap — the map stores optical
            // depth measured FROM the light, so sampling it at the point where
            // the cone stops is exactly the remainder, with no double count.
            // Both halves are in physical (extinction-multiplied) units by the
            // time they are added, and the map's own gate makes the second term
            // 0 when its cascade is off.
            float nearRange = cloudLightNearRange();
            float lightOD = cloudLightOpticalDepth(samplePos, u_CloudSunDir.xyz, lightSteps, nearRange) * kExtinction;
            lightOD += volumetricShadowOpticalDepth(samplePos + u_CloudSunDir.xyz * nearRange - u_RenderOrigin,
                                                    VSM_CASCADE_CLOUD);
            float sunTrans = cloudBeerPowder(lightOD, u_CloudLight.w);
            sunTrans += u_CloudLight.z * 0.6 * cloudBeerPowder(lightOD * 0.25, 0.0);
            // 0.55: artistic single-scatter albedo — untamed, the sun term
            // integrates to ~6+ radiance and whites out the deck (tuned live).
            vec3 sunLight = u_CloudSunColor.rgb * (sunTrans * phase * u_CloudLight.x * 0.55);

            // Ambient from the sky estimate, brighter toward the cloud tops.
            // The 0.25..0.75 band is tuned live: a full-strength ambient
            // integrates to a blown-out white deck over the layer thickness.
            float heightFrac = cloudHeightFraction(samplePos.y);
            vec3 ambient = u_CloudAmbient.rgb * (u_CloudLight.y * mix(0.25, 0.75, heightFrac));

            // Energy-conserving step integration (Hillaire).
            vec3 stepScatter = (sunLight + ambient) * (density * wetnessDarken);
            inscatter += transmittance * stepScatter * ((1.0 - stepTrans) / max(extinction, 1.0e-5));

            transmittance *= stepTrans;
            if (transmittance < 0.01)
                break;
        }
        t += stepLen;
    }

    // Distance fade: hand the far field to the sky/fog instead of a hard cut.
    float distanceFade = smoothstep(kMaxMarchDistance, kMaxMarchDistance * 0.6, tStart);
    inscatter *= distanceFade;
    transmittance = mix(1.0, transmittance, distanceFade);

    o_Cloud = vec4(max(inscatter, vec3(0.0)), clamp(transmittance, 0.0, 1.0));
}
