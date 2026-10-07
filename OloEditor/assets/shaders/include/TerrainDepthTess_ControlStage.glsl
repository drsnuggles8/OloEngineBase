layout(vertices = 3) out;

layout(location = 0) in vec3 v_Position[];
layout(location = 1) in vec2 v_TexCoord[];

layout(location = 0) out vec3 tc_Position[];
layout(location = 1) out vec2 tc_TexCoord[];

// Terrain UBO (binding 10)
#include "TerrainParamsBlock.glsl"

void main()
{
    tc_Position[gl_InvocationID] = v_Position[gl_InvocationID];
    tc_TexCoord[gl_InvocationID] = v_TexCoord[gl_InvocationID];

    if (gl_InvocationID == 0)
    {
        if (u_TessFactors2.w > 0.5)
        {
            // Use quadtree-provided tessellation factors
            gl_TessLevelInner[0] = u_TessFactors.x;
            gl_TessLevelOuter[0] = u_TessFactors.y;
            gl_TessLevelOuter[1] = u_TessFactors.z;
            gl_TessLevelOuter[2] = u_TessFactors.w;
        }
        else
        {
            // Shadow maps use moderate fixed tessellation
            gl_TessLevelInner[0] = 4.0;
            gl_TessLevelOuter[0] = 4.0;
            gl_TessLevelOuter[1] = 4.0;
            gl_TessLevelOuter[2] = 4.0;
        }
    }
}
