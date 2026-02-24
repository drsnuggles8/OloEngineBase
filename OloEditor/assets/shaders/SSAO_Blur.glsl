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

// SSAO Bilateral Blur — Edge-aware blur that preserves depth discontinuities.
// Uses a depth-dependent weight to prevent AO bleeding across geometric edges.
// Runs at half-resolution matching the SSAO generation pass.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Raw SSAO texture (R channel = AO value)
layout(binding = 0) uniform sampler2D u_SSAOTexture;

// Scene depth for edge awareness
layout(binding = 19) uniform sampler2D u_DepthTexture;

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
