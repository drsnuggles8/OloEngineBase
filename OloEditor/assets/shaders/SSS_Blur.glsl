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

// SSS Blur Pass — screen-space Gaussian blur masked by alpha (SSS weight)
// Reads the scene color attachment (RGBA16F), blurs only pixels with alpha > 0
// and blends the result back using the alpha mask.
//
// This is the CONSUMER of the alpha-channel SSS mask produced by PBR shaders.
// It ALWAYS resets alpha to 1.0 on output, completing the produce-consume-reset
// lifecycle (see SnowCommon.glsl for the full contract).

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

// Scene color (RGBA16F, attachment 0)
layout(binding = 0) uniform sampler2D u_SceneColor;
// Scene depth for bilateral edge-aware filtering
layout(binding = 19) uniform sampler2D u_SceneDepth;

// SSS UBO (binding 14)
layout(std140, binding = 14) uniform SSSParams {
    vec4 u_SSSBlurParams;   // (blurRadius, blurFalloff, screenWidth, screenHeight)
    vec4 u_SSSFlags;        // (enabled, pad, pad, pad)
};

// Bilateral Gaussian weights for a 9-tap kernel
const float gaussWeights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

float linearizeDepth(float d, float near, float far)
{
    return near * far / (far - d * (far - near));
}

void main()
{
    // Early out if SSS blur is disabled — still reset alpha to 1.0
    // so the SSS mask doesn't leak into downstream passes
    if (u_SSSFlags.x < 0.5)
    {
        vec4 c = texture(u_SceneColor, v_TexCoord);
        o_Color = vec4(c.rgb, 1.0);
        return;
    }

    vec4 centerSample = texture(u_SceneColor, v_TexCoord);
    float sssMask = centerSample.a;

    // No SSS on this pixel — pass through with alpha reset to 1.0
    if (sssMask < 0.001)
    {
        o_Color = vec4(centerSample.rgb, 1.0);
        return;
    }

    float blurRadius = u_SSSBlurParams.x;
    float blurFalloff = u_SSSBlurParams.y;
    vec2 texelSize = vec2(1.0 / u_SSSBlurParams.z, 1.0 / u_SSSBlurParams.w);

    float centerDepth = texture(u_SceneDepth, v_TexCoord).r;

    vec3 result = centerSample.rgb * gaussWeights[0];
    float totalWeight = gaussWeights[0];

    // Two-pass separable blur (combined into single pass with diagonal sampling for simplicity)
    // Horizontal + vertical averaged
    for (int i = 1; i < 5; ++i)
    {
        float offset = float(i) * blurRadius;

        // Horizontal samples
        vec2 offsetH = vec2(texelSize.x * offset, 0.0);
        vec4 sampleH1 = texture(u_SceneColor, v_TexCoord + offsetH);
        vec4 sampleH2 = texture(u_SceneColor, v_TexCoord - offsetH);

        // Vertical samples
        vec2 offsetV = vec2(0.0, texelSize.y * offset);
        vec4 sampleV1 = texture(u_SceneColor, v_TexCoord + offsetV);
        vec4 sampleV2 = texture(u_SceneColor, v_TexCoord - offsetV);

        // Bilateral depth weights (edge-aware: don't blur across depth discontinuities)
        float depthH1 = texture(u_SceneDepth, v_TexCoord + offsetH).r;
        float depthH2 = texture(u_SceneDepth, v_TexCoord - offsetH).r;
        float depthV1 = texture(u_SceneDepth, v_TexCoord + offsetV).r;
        float depthV2 = texture(u_SceneDepth, v_TexCoord - offsetV).r;

        float wH1 = exp(-abs(depthH1 - centerDepth) * blurFalloff) * gaussWeights[i] * sampleH1.a;
        float wH2 = exp(-abs(depthH2 - centerDepth) * blurFalloff) * gaussWeights[i] * sampleH2.a;
        float wV1 = exp(-abs(depthV1 - centerDepth) * blurFalloff) * gaussWeights[i] * sampleV1.a;
        float wV2 = exp(-abs(depthV2 - centerDepth) * blurFalloff) * gaussWeights[i] * sampleV2.a;

        result += sampleH1.rgb * wH1 + sampleH2.rgb * wH2;
        result += sampleV1.rgb * wV1 + sampleV2.rgb * wV2;
        totalWeight += wH1 + wH2 + wV1 + wV2;
    }

    result /= totalWeight;

    // Blend blurred result with original based on SSS mask intensity
    vec3 finalColor = mix(centerSample.rgb, result, sssMask);

    // Reset alpha to 1.0 — the SSS mask has been consumed by this pass
    o_Color = vec4(finalColor, 1.0);
}
