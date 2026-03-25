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

layout(binding = 0) uniform sampler2D u_SceneColor;
layout(binding = 1) uniform isampler2D u_EntityID;

layout(std140, binding = 27) uniform SelectionOutlineUBO
{
    vec4  u_OutlineColor;    // rgb = color, a = opacity
    vec4  u_TexelSize;       // xy = 1/width, 1/height
    int   u_SelectedCount;   // Number of selected entities
    int   u_OutlineWidth;    // Outline width in texels
    int   _pad0;
    int   _pad1;
    ivec4 u_SelectedIDs[16]; // 64 entity IDs packed as ivec4 (4 per vec)
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
    vec4 sceneColor = texture(u_SceneColor, v_TexCoord);
    ivec2 coord = ivec2(gl_FragCoord.xy);
    int centerID = texelFetch(u_EntityID, coord, 0).r;
    bool centerSelected = isSelected(centerID);

    // Sample cross neighbors at ±OutlineWidth texels
    bool hasEdge = false;
    int w = u_OutlineWidth;

    int rightID  = texelFetch(u_EntityID, coord + ivec2( w, 0), 0).r;
    int leftID   = texelFetch(u_EntityID, coord + ivec2(-w, 0), 0).r;
    int topID    = texelFetch(u_EntityID, coord + ivec2(0,  w), 0).r;
    int bottomID = texelFetch(u_EntityID, coord + ivec2(0, -w), 0).r;

    if (centerSelected)
    {
        // Edge: center is selected but at least one neighbor is not selected or is a different entity
        hasEdge = !isSelected(rightID) || !isSelected(leftID) ||
                  !isSelected(topID)   || !isSelected(bottomID);
    }
    else
    {
        // Edge: center is NOT selected but at least one neighbor IS selected
        hasEdge = isSelected(rightID) || isSelected(leftID) ||
                  isSelected(topID)   || isSelected(bottomID);
    }

    if (hasEdge)
    {
        // Alpha-blend outline color over scene color
        o_Color = vec4(mix(sceneColor.rgb, u_OutlineColor.rgb, u_OutlineColor.a), sceneColor.a);
    }
    else
    {
        o_Color = sceneColor;
    }
}
