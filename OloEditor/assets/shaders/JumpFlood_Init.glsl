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

layout(binding = 0) uniform isampler2D u_EntityID;

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
