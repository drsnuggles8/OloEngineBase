#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 0) uniform TransformUBO
{
    mat4 u_ViewProjection;
    mat4 u_Model;
};

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Texture;

layout(location = 0) out vec4 color;

void main()
{
    vec4 texColor = texture(u_Texture, v_TexCoord);
    
    // Discard fragment if alpha is very low (completely transparent)
    if(texColor.a < 0.01)
        discard;
        
    color = texColor;
}