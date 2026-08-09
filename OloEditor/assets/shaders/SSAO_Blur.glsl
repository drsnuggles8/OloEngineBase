#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 7 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
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

// SSAO Bilateral Blur — Edge-aware blur that preserves depth discontinuities.
// Uses a depth-dependent weight to prevent AO bleeding across geometric edges.
// Runs at half-resolution matching the SSAO generation pass.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Raw SSAO texture (R channel = AO value)
#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). SSAO.glsl (pass 1) is
// a separate program and was converted earlier; this is pass 2's own shader.
#ifdef OLO_BINDLESS
#define u_SSAOTexture OLO_HEAP_TEX_2D(0)
#define u_DepthTexture OLO_HEAP_TEX_2D(19) // TEX_POSTPROCESS_DEPTH
#else
layout(binding = 0) uniform sampler2D u_SSAOTexture;

// Scene depth for edge awareness
layout(binding = 19) uniform sampler2D u_DepthTexture;
#endif

// SSAO UBO (binding 9) — need inverse projection for linear depth
layout(std140, binding = 9) uniform SSAOUBO
{
    float u_Radius;
    float u_Bias;
    float u_Intensity;
    int   u_Samples;

    int   u_ScreenWidth;
    int   u_ScreenHeight;
    int   u_DebugView;
    float _pad1;

    mat4  u_Projection;
    mat4  u_InverseProjection;
};

// Linearize depth using projection matrix parameters
float linearizeDepth(float depth)
{
    // Extract near/far from projection matrix
    // For standard perspective: P[2][2] = -(f+n)/(f-n), P[3][2] = -2fn/(f-n)
    float A = u_Projection[2][2];
    float B = u_Projection[3][2];
    float ndc = depth * 2.0 - 1.0;
    return B / (A + ndc);
}

void main()
{
    vec2 texelSize = 1.0 / vec2(textureSize(u_SSAOTexture, 0));

    float centerAO = texture(u_SSAOTexture, v_TexCoord).r;
    float centerDepth = linearizeDepth(texture(u_DepthTexture, v_TexCoord).r);

    // Bilateral filter: 4x4 Gaussian-weighted kernel with depth-based edge stopping
    float totalWeight = 0.0;
    float totalAO = 0.0;

    // Depth sensitivity — controls how much depth differences reduce blur weight
    // Larger values = more sensitive to depth edges = sharper AO at edges
    float depthSigma = 0.5; // In linear depth units

    for (int x = -2; x <= 1; ++x)
    {
        for (int y = -2; y <= 1; ++y)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            vec2 sampleUV = v_TexCoord + offset;

            float sampleAO = texture(u_SSAOTexture, sampleUV).r;
            float sampleDepth = linearizeDepth(texture(u_DepthTexture, sampleUV).r);

            // Spatial weight (Gaussian, sigma ~1.5 for 4x4 kernel)
            float spatialDist = length(vec2(float(x), float(y)));
            float spatialWeight = exp(-spatialDist * spatialDist / 4.5);

            // Range weight based on depth difference (bilateral term)
            float depthDiff = abs(centerDepth - sampleDepth);
            float rangeWeight = exp(-depthDiff * depthDiff / (2.0 * depthSigma * depthSigma));

            float w = spatialWeight * rangeWeight;
            totalAO += sampleAO * w;
            totalWeight += w;
        }
    }

    float result = (totalWeight > 0.0) ? totalAO / totalWeight : centerAO;
    o_Color = vec4(result, 0.0, 0.0, 1.0);
}
