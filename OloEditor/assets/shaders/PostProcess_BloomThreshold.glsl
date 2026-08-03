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

void main()
{
    vec3 color = texture(u_Texture, v_TexCoord).rgb;

    // Extract bright pixels above threshold
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    vec3 bright = max(color - vec3(u_BloomThreshold), vec3(0.0));
    // Soft knee: fade around threshold
    float softness = brightness - u_BloomThreshold + 0.5;
    softness = clamp(softness, 0.0, 1.0);
    softness = softness * softness;
    bright = color * softness * step(u_BloomThreshold, brightness + 0.5);

    o_Color = vec4(bright, 1.0);
}
