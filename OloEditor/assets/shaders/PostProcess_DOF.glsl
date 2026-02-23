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

layout(binding = 0) uniform sampler2D u_Texture;     // Scene color (HDR)
layout(binding = 19) uniform sampler2D u_DepthTexture; // Scene depth

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

float linearizeDepth(float d)
{
    float z = d * 2.0 - 1.0; // NDC
    return (2.0 * u_Near * u_Far) / (u_Far + u_Near - z * (u_Far - u_Near));
}

#define DOF_SAMPLES 16
// Golden angle spiral for disc sampling
const float GOLDEN_ANGLE = 2.39996323;

void main()
{
    vec2 texelSize = vec2(u_InverseScreenWidth, u_InverseScreenHeight);

    float depth = texture(u_DepthTexture, v_TexCoord).r;
    float linearDepth = linearizeDepth(depth);

    // Calculate circle of confusion (CoC)
    float coc = abs(linearDepth - u_DOFFocusDistance) / u_DOFFocusRange;
    coc = clamp(coc, 0.0, 1.0);

    float blurRadius = coc * u_DOFBokehRadius;

    if (blurRadius < 0.5)
    {
        // No blur needed
        o_Color = vec4(texture(u_Texture, v_TexCoord).rgb, 1.0);
        return;
    }

    // Gather samples in a disc pattern
    vec3 result = vec3(0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < DOF_SAMPLES; i++)
    {
        float angle = float(i) * GOLDEN_ANGLE;
        float r = sqrt(float(i) + 0.5) / sqrt(float(DOF_SAMPLES));

        vec2 offset = vec2(cos(angle), sin(angle)) * r * blurRadius * texelSize;
        vec2 sampleUV = v_TexCoord + offset;

        // Sample the scene color
        vec3 sampleColor = texture(u_Texture, sampleUV).rgb;

        // Depth-aware weighting to prevent near objects bleeding into far
        float sampleDepth = linearizeDepth(texture(u_DepthTexture, sampleUV).r);
        float sampleCoC = abs(sampleDepth - u_DOFFocusDistance) / u_DOFFocusRange;
        sampleCoC = clamp(sampleCoC, 0.0, 1.0);

        // Near-field: allow bleeding in front of sharp objects
        float weight = (sampleDepth > linearDepth) ? sampleCoC : coc;
        weight = max(weight, 0.01);

        result += sampleColor * weight;
        totalWeight += weight;
    }

    result /= totalWeight;
    o_Color = vec4(result, 1.0);
}
