#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture;

layout(location = 0) out vec4 o_Color;

void main()
{
    o_Color = texture(u_Texture, v_TexCoord);
} 