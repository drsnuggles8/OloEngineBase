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

// 13-tap box downsample (reduces fireflies via partial Karis average on first pass)
void main()
{
    vec2 uv = v_TexCoord;
    vec2 d = vec2(u_TexelSizeX, u_TexelSizeY);

    // Take 13 samples in a cross/box pattern for better quality downsample
    // Pattern: 4 corner samples + 4 edge samples + 1 center + 4 diagonal
    vec3 a = texture(u_Texture, uv + vec2(-2.0, -2.0) * d).rgb;
    vec3 b = texture(u_Texture, uv + vec2( 0.0, -2.0) * d).rgb;
    vec3 c = texture(u_Texture, uv + vec2( 2.0, -2.0) * d).rgb;

    vec3 d0 = texture(u_Texture, uv + vec2(-1.0, -1.0) * d).rgb;
    vec3 e = texture(u_Texture, uv).rgb;
    vec3 f = texture(u_Texture, uv + vec2( 1.0, -1.0) * d).rgb;

    vec3 g = texture(u_Texture, uv + vec2(-2.0,  0.0) * d).rgb;
    vec3 h = texture(u_Texture, uv + vec2( 0.0,  0.0) * d).rgb;
    vec3 i = texture(u_Texture, uv + vec2( 2.0,  0.0) * d).rgb;

    vec3 j = texture(u_Texture, uv + vec2(-1.0,  1.0) * d).rgb;
    vec3 k = texture(u_Texture, uv + vec2( 0.0,  2.0) * d).rgb;
    vec3 l = texture(u_Texture, uv + vec2( 1.0,  1.0) * d).rgb;

    vec3 m = texture(u_Texture, uv + vec2(-2.0,  2.0) * d).rgb;

    // Weighted average (following the Call of Duty: Advanced Warfare method)
    vec3 result = e * 0.125;
    result += (d0 + f + j + l) * 0.125;
    result += (a + c + m + k) * 0.03125; // corner 4 get less weight
    result += (b + g + i + h) * 0.0625;

    o_Color = vec4(result, 1.0);
}
