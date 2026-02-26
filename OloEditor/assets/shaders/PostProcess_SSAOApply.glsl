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

// SSAO Apply — Modulates scene color by the SSAO occlusion factor.
// Uses depth-aware bilateral upsampling to prevent half-res AO from bleeding
// across depth discontinuities (which causes blurry dark halos).

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Scene HDR color (ping-pong source)
layout(binding = 0) uniform sampler2D u_Texture;

// Blurred SSAO result (R channel = AO value, 0 = full occlusion, 1 = no occlusion)
// This texture is at HALF resolution.
layout(binding = 20) uniform sampler2D u_SSAOTexture;

// Full-res scene depth for bilateral upsampling
layout(binding = 19) uniform sampler2D u_DepthTexture;

// SSAO UBO (binding 9) — we read intensity and debug flag from here
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

// Linearize depth for bilateral comparison
float linearizeDepth(float depth)
{
    float A = u_Projection[2][2];
    float B = u_Projection[3][2];
    float ndc = depth * 2.0 - 1.0;
    return B / (A + ndc);
}

// Depth-aware bilateral upsample: sample 4 nearest half-res texels and
// weight by depth similarity to avoid bleeding AO across depth edges.
float bilateralUpsampleAO(vec2 uv)
{
    vec2 ssaoTexSize = vec2(textureSize(u_SSAOTexture, 0));
    vec2 texelSize = 1.0 / ssaoTexSize;

    // Map full-res UV to half-res texel coordinates
    vec2 halfResCoord = uv * ssaoTexSize - 0.5;
    vec2 baseCoord = floor(halfResCoord);
    vec2 frac = halfResCoord - baseCoord;

    // Full-res center depth for bilateral weighting
    float centerDepth = linearizeDepth(texture(u_DepthTexture, uv).r);

    // Depth sensitivity — controls edge sharpness
    float depthSigma = 0.02 * abs(centerDepth);
    depthSigma = max(depthSigma, 0.01);
    float invTwoSigmaSq = 1.0 / (2.0 * depthSigma * depthSigma);

    float totalWeight = 0.0;
    float totalAO = 0.0;

    // Sample 2x2 nearest half-res texels with bilinear + depth weights
    for (int dy = 0; dy <= 1; ++dy)
    {
        for (int dx = 0; dx <= 1; ++dx)
        {
            vec2 sampleUV = (baseCoord + vec2(float(dx), float(dy)) + 0.5) * texelSize;
            sampleUV = clamp(sampleUV, texelSize * 0.5, 1.0 - texelSize * 0.5);

            float sampleAO = texture(u_SSAOTexture, sampleUV).r;

            // Approximate depth at this half-res sample location using full-res depth
            float sampleDepth = linearizeDepth(texture(u_DepthTexture, sampleUV).r);

            // Bilinear weight
            float bx = (dx == 0) ? (1.0 - frac.x) : frac.x;
            float by = (dy == 0) ? (1.0 - frac.y) : frac.y;
            float bilinearWeight = bx * by;

            // Depth weight — suppress samples across depth discontinuities
            float depthDiff = centerDepth - sampleDepth;
            float depthWeight = exp(-depthDiff * depthDiff * invTwoSigmaSq);

            float w = bilinearWeight * depthWeight;
            totalAO += sampleAO * w;
            totalWeight += w;
        }
    }

    return (totalWeight > 0.001) ? totalAO / totalWeight : texture(u_SSAOTexture, uv).r;
}

void main()
{
    vec3 sceneColor = texture(u_Texture, v_TexCoord).rgb;

    if (u_DebugView != 0)
    {
        float ao = bilateralUpsampleAO(v_TexCoord);
        o_Color = vec4(vec3(ao), 1.0);
        return;
    }

    float ao = bilateralUpsampleAO(v_TexCoord);

    // Mix between full color and AO-modulated color based on intensity
    vec3 result = sceneColor * mix(1.0, ao, u_Intensity);

    o_Color = vec4(result, 1.0);
}
