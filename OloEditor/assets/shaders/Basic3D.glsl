#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Per-draw transform / normal matrix / entity id come from the instance SSBO
// at binding = 15. The shader body keeps reading `u_Model` etc. via the
// macros declared in InstanceBlock.glsl, so only the resource declaration
// differs from the legacy ModelMatrices UBO at binding = 3.
#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec3 v_Color;
layout(location = 1) out vec2 v_TexCoord;

void main()
{
    OLO_INSTANCE_FORWARD();
    v_Color = a_Color;
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec3 v_Color;
layout(location = 1) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture;

layout(location = 0) out vec4 FragColor;

void main()
{
    vec4 texColor = texture(u_Texture, v_TexCoord);
    FragColor = texColor * vec4(v_Color, 1.0);
}
