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

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). The BODY below is
// byte-identical between the two variants. Note the INTEGER form: the entity-ID
// target is R32I, and a heap handle carries no type — reading it as a plain
// sampler2D would reinterpret the same uvec2 as a float texture.
#ifdef OLO_BINDLESS
#define u_EntityID OLO_HEAP_TEX_2D_INT(0)
#else
layout(binding = 0) uniform isampler2D u_EntityID;
#endif

layout(std140, binding = 27) uniform SelectionOutlineUBO
{
    vec4  u_OutlineColor;
    vec4  u_TexelSize;       // xy = 1/width, 1/height
    int   u_SelectedCount;
    int   u_OutlineWidth;
    int   _pad0;
    int   _pad1;
    ivec4 u_SelectedIDs[16]; // 64 entity IDs packed as ivec4
};

// Check if an entity ID is in the selected set
bool isSelected(int id)
{
    if (id == -1)
        return false;

    for (int i = 0; i < (u_SelectedCount + 3) / 4; ++i)
    {
        if (u_SelectedIDs[i].x == id || u_SelectedIDs[i].y == id ||
            u_SelectedIDs[i].z == id || u_SelectedIDs[i].w == id)
            return true;
    }
    return false;
}

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    int entityID = texelFetch(u_EntityID, coord, 0).r;
    bool selected = isSelected(entityID);

    // Initialize distance field:
    // xy = offset to nearest seed (start far away)
    // z  = squared screen-space distance
    // w  = 1.0 if selected (inside), 0.0 if not (outside)
    float sqDist = dot(vec2(100.0), vec2(100.0));
    o_Color = vec4(100.0, 100.0, sqDist, selected ? 1.0 : 0.0);
}
