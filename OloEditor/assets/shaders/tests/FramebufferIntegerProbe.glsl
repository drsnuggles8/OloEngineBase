#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
void main() { gl_Position = vec4(a_Position, 1.0); }
#type fragment
#version 460 core
layout(binding = 12) uniform isampler2D u_Ids;
layout(location = 0) out vec4 o_Color;
void main() { o_Color = vec4(float(texelFetch(u_Ids, ivec2(gl_FragCoord.xy), 0).r), 0.0, 0.0, 1.0); }
