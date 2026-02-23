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

#define TONEMAP_NONE      0
#define TONEMAP_REINHARD  1
#define TONEMAP_ACES      2
#define TONEMAP_UNCHARTED2 3

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

    // Apply exposure
    hdrColor *= u_Exposure;

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
