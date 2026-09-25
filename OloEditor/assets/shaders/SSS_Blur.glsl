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

// =============================================================================
// The snow subsurface blur (issue #1451) — SSSRenderPass.
//
// Snow scatters light below its surface, so the DIFFUSE half of a snow pixel's
// lighting is spread over its snow neighbours. The half arrives in the
// diffusion hand-off lane (scene attachment 4): .rgb the diffuse radiance, .a
// the snow weight in the lane's negative range (include/SnowDiffusionCommon.glsl
// — skin slots are positive and read here as "not snow").
//
// The pass draws into scene colour with an ADDITIVE blend and emits
//
//     strength * (blur(diffuse) - diffuse),  strength = weight * SSSIntensity
//
// so the blurred fraction of the diffuse half replaces its sharp self and every
// other term — specular, sparkle, emission, anything that is not snow — is left
// exactly as the lit pass wrote it. A pixel that hands over no snow emits zero,
// which is why the blur can no longer touch a surface without snow (it used to
// read its mask from scene alpha, which every non-snow writer set to 1).
//
// Taps are fetched by TEXEL (the offsets are whole texels), so the weight lane
// is never interpolated between a snow texel and a skin or empty one.
// =============================================================================

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

#include "include/BindlessHeap.glsl"
#include "include/SnowDiffusionCommon.glsl"

// Heap-bindless conversion (issue #691): the body is identical between the two
// variants, and each name is the slot SSSRenderPass binds.
#ifdef OLO_BINDLESS
#define u_SnowDiffuse OLO_HEAP_TEX_2D(0) // TEX_DIFFUSE: scene attachment 4
#define u_SceneDepth OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 0) uniform sampler2D u_SnowDiffuse; // scene attachment 4, the hand-off lane
layout(binding = 19) uniform sampler2D u_SceneDepth;  // scene depth, for the bilateral weight
#endif

// SSS UBO (binding 14) — SSSUBOData.
layout(std140, binding = 14) uniform SSSParams {
    vec4 u_SSSBlurParams; // (blurRadius texels, depthFalloff, width, height)
    vec4 u_SSSFlags;      // (enabled, strength = SSSIntensity, pad, pad)
};

// Gaussian weights for the 9-tap arms of the cross (centre + 4 each way).
const float gaussWeights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

// One neighbour's contribution: its diffuse half, weighted by the Gaussian, by
// depth similarity (don't blur across a silhouette) and by ITS OWN snow weight
// (only snow scatters into snow).
void oloSnowBlurTap(ivec2 texel, ivec2 size, float centreDepth, float falloff, float gauss, inout vec3 sum,
                    inout float total)
{
    texel = clamp(texel, ivec2(0), size - ivec2(1));
    vec4 tap = texelFetch(u_SnowDiffuse, texel, 0);
    float depth = texelFetch(u_SceneDepth, texel, 0).r;
    float weight = exp(-abs(depth - centreDepth) * falloff) * gauss * oloSnowDiffusionWeight(tap.a);
    sum += tap.rgb * weight;
    total += weight;
}

void main()
{
    ivec2 size = textureSize(u_SnowDiffuse, 0);
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - ivec2(1));
    vec4 centre = texelFetch(u_SnowDiffuse, pixel, 0);
    float snowWeight = oloSnowDiffusionWeight(centre.a);
    float strength = snowWeight * clamp(u_SSSFlags.y, 0.0, 1.0);

    // Not snow, blur off, or nothing to scatter: add exactly nothing.
    if (u_SSSFlags.x < 0.5 || !(snowWeight > OLO_SNOW_MIN_WEIGHT) || !(strength > 0.0))
    {
        o_Color = vec4(0.0);
        return;
    }

    float blurRadius = max(u_SSSBlurParams.x, 0.0);
    float falloff = max(u_SSSBlurParams.y, 0.0);
    float centreDepth = texelFetch(u_SceneDepth, pixel, 0).r;

    // The centre tap carries its own snow weight like every other, so a
    // lightly-covered pixel's diffuse half weighs in proportion to its cover.
    vec3 sum = centre.rgb * (gaussWeights[0] * snowWeight);
    float total = gaussWeights[0] * snowWeight;
    for (int i = 1; i < 5; ++i)
    {
        int offset = int(round(float(i) * blurRadius));
        oloSnowBlurTap(pixel + ivec2(offset, 0), size, centreDepth, falloff, gaussWeights[i], sum, total);
        oloSnowBlurTap(pixel - ivec2(offset, 0), size, centreDepth, falloff, gaussWeights[i], sum, total);
        oloSnowBlurTap(pixel + ivec2(0, offset), size, centreDepth, falloff, gaussWeights[i], sum, total);
        oloSnowBlurTap(pixel - ivec2(0, offset), size, centreDepth, falloff, gaussWeights[i], sum, total);
    }

    vec3 blurred = (total > 0.0) ? sum / total : centre.rgb;
    // Alpha 0: the blend leaves scene alpha alone (ZERO/ONE).
    o_Color = vec4(strength * (blurred - centre.rgb), 0.0);
}
