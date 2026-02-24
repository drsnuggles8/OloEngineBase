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
// Runs as the first step in the post-process chain (before bloom, tone mapping, etc.)

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Scene HDR color (ping-pong source)
layout(binding = 0) uniform sampler2D u_Texture;

// Blurred SSAO result (R channel = AO value, 0 = full occlusion, 1 = no occlusion)
layout(binding = 20) uniform sampler2D u_SSAOTexture;

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

void main()
{
    vec3 sceneColor = texture(u_Texture, v_TexCoord).rgb;
    float ao = texture(u_SSAOTexture, v_TexCoord).r;

    if (u_DebugView != 0)
    {
        // Debug: show raw AO as grayscale
        o_Color = vec4(vec3(ao), 1.0);
        return;
    }

    // Mix between full color and AO-modulated color based on intensity
    vec3 result = sceneColor * mix(1.0, ao, u_Intensity);

    o_Color = vec4(result, 1.0);
}
