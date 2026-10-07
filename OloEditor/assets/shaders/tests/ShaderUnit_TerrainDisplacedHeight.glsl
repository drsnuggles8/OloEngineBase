#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
void main()
{
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core
#include "include/TerrainDisplacedHeight.glsl"
layout(location = 0) out vec4 o_Color;
void main()
{
    o_Color = vec4(oloTerrainDisplacedHeight(10.0, 2.0, 4.0, 0.0),
                   oloTerrainDisplacedHeight(10.0, 2.0, 4.0, 0.5),
                   oloTerrainDisplacedHeight(10.0, 2.0, 4.0, 1.0),
                   oloTerrainDisplacedHeight(10.0, 0.0, 4.0, 0.0));
}
