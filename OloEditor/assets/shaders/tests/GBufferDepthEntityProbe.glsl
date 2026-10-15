#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
void main() { gl_Position = vec4(a_Position, 1.0); }

#type fragment
#version 460 core
layout(location = 0) out vec4 o_Albedo;
layout(location = 1) out vec4 o_Normal;
layout(location = 2) out vec4 o_Emissive;
layout(location = 3) out vec4 o_Velocity;
layout(location = 4) out int o_EntityID;
layout(location = 5) out vec4 o_BakedGI;
void main()
{
    o_Albedo = vec4(0.5, 0.5, 0.5, 1.0);
    o_Normal = vec4(0.0, 0.0, 0.5, 1.0);
    o_Emissive = vec4(0.0);
    o_Velocity = vec4(0.0, 0.0, 1.0, 0.0);
    o_BakedGI = vec4(0.0);
    gl_FragDepth = gl_SampleID == 0 ? 0.75 : 0.15;
    o_EntityID = gl_SampleID == 0 ? 17 : 29;
}
