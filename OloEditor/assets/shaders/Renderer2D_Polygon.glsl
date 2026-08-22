//--------------------------
// - OloEngine -
// Renderer2D Polygon Shader
// --------------------------

#type vertex
#version 450 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is the Renderer2D polygon batch (Renderer2D.cpp
// PolygonVertex, 32 B / 8 words): vec3 Position @0, vec4 Color @12, int
// EntityID @28. The stream carries an INT attribute, so the pull buffer is
// declared uint and floats are decoded with uintBitsToFloat. The GL
// attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    uint v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in int a_EntityID;
#endif

layout(std140, binding = 0) uniform Camera
{
    mat4 u_ViewProjection;
};

layout(location = 0) out vec4 v_Color;
layout(location = 1) out flat int v_EntityID;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(uintBitsToFloat(b_Vertices.v[vertBase + 0]), uintBitsToFloat(b_Vertices.v[vertBase + 1]), uintBitsToFloat(b_Vertices.v[vertBase + 2]));
    vec4 a_Color = vec4(uintBitsToFloat(b_Vertices.v[vertBase + 3]), uintBitsToFloat(b_Vertices.v[vertBase + 4]), uintBitsToFloat(b_Vertices.v[vertBase + 5]), uintBitsToFloat(b_Vertices.v[vertBase + 6]));
    int a_EntityID = int(b_Vertices.v[vertBase + 7]);
#endif
    v_Color = a_Color;
    v_EntityID = a_EntityID;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec4 v_Color;
layout(location = 1) in flat int v_EntityID;

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

void main()
{
    o_Color = v_Color;
    o_EntityID = v_EntityID;
    o_ViewNormal = vec2(-2.0);
}
