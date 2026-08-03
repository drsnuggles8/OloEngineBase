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

// 9-tap tent filter upsample
void main()
{
    vec2 uv = v_TexCoord;
    vec2 d = vec2(u_TexelSizeX, u_TexelSizeY);

    // 3x3 tent filter
    vec3 result = vec3(0.0);
    result += texture(u_Texture, uv + vec2(-1.0, -1.0) * d).rgb * 1.0;
    result += texture(u_Texture, uv + vec2( 0.0, -1.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv + vec2( 1.0, -1.0) * d).rgb * 1.0;
    result += texture(u_Texture, uv + vec2(-1.0,  0.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv).rgb * 4.0;
    result += texture(u_Texture, uv + vec2( 1.0,  0.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv + vec2(-1.0,  1.0) * d).rgb * 1.0;
    result += texture(u_Texture, uv + vec2( 0.0,  1.0) * d).rgb * 2.0;
    result += texture(u_Texture, uv + vec2( 1.0,  1.0) * d).rgb * 1.0;
    result /= 16.0;

    o_Color = vec4(result, 1.0);
}
