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

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Cloud;

#include "include/NoiseCommon.glsl"
#include "include/CloudscapeCommon.glsl"

// Full-res scene depth (TEX_POSTPROCESS_DEPTH).
layout(binding = 19) uniform sampler2D u_DepthTexture;

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

// Optical depth toward the light through the layer: short cone of
// exponentially spaced cheap density taps (Schneider light march).
float cloudLightOpticalDepth(vec3 worldPos, vec3 towardLight, int lightSteps)
{
    float layerThickness = u_CloudLayer.y - u_CloudLayer.x;
    float maxT = min(layerThickness, 1400.0);
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
            float lightOD = cloudLightOpticalDepth(samplePos, u_CloudSunDir.xyz, lightSteps) * kExtinction;
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
