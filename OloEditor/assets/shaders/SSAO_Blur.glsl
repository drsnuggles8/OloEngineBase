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

// SSAO Blur — Simple box blur (4x4) to smooth noise from SSAO generation.
// Edge-aware (bilateral) blur is not needed at half-res since the noise pattern
// is already localized. A simple blur provides sufficient quality.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Raw SSAO texture (R channel = AO value)
layout(binding = 0) uniform sampler2D u_SSAOTexture;

void main()
{
    vec2 texelSize = 1.0 / vec2(textureSize(u_SSAOTexture, 0));

    float result = 0.0;
    for (int x = -2; x < 2; ++x)
    {
        for (int y = -2; y < 2; ++y)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            result += texture(u_SSAOTexture, v_TexCoord + offset).r;
        }
    }
    result /= 16.0; // 4x4 kernel

    o_Color = vec4(result, 0.0, 0.0, 1.0);
}
