#type vertex
#version 450 core

#ifdef OLO_VULKAN
// #691 Phase 8 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is Renderer3D::DrawQuad, which draws
// s_Data.QuadMesh = MeshPrimitives::CreatePlane(1,1) — the engine `Vertex`
// (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24), so the stride is
// 8 floats. The GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
#endif

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec2 v_TexCoord;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
    OLO_INSTANCE_FORWARD();
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * u_Model * vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#else
layout(binding = 0) uniform sampler2D u_Texture;
#endif

layout(location = 0) out vec4 color;

void main()
{
    vec4 texColor = texture(u_Texture, v_TexCoord);

    // Discard fragment if alpha is very low (completely transparent)
    if(texColor.a < 0.01)
        discard;

    color = texColor;
}
