#type vertex
#version 450 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is Renderer3D::DrawLightCube, which draws
// s_Data.CubeMesh = MeshPrimitives::CreateCube() — the engine `Vertex`
// (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24), so the stride is
// 8 floats even though this stage only needs the position. The GL attribute
// branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
};

// This shader's consuming stage never reads v_InstanceIndex — declare no
// varying (a written-but-unconsumed output is a per-pipeline Vulkan
// validation interface warning).
#define OLO_INSTANCE_NO_FORWARD 1
#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec4 v_ClipPosCurr;
layout(location = 1) out vec4 v_ClipPosPrev;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    OLO_INSTANCE_FORWARD();
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    vec4 clipCurr = u_ViewProjection * worldPos;
    vec4 prevWorldPos = u_PrevModel * vec4(a_Position, 1.0);
    vec4 clipPrev = u_PrevViewProjection * prevWorldPos;

    v_ClipPosCurr = clipCurr;
    v_ClipPosPrev = clipPrev;

    gl_Position = clipCurr;
}

#type fragment
#version 450 core

layout(location = 0) in vec4 v_ClipPosCurr;
layout(location = 1) in vec4 v_ClipPosPrev;

layout(location = 0) out vec4 FragColor;
// Scene FB RT3 velocity — light gizmos are rigid objects so per-object
// motion plus camera motion fully describes their screen-space trajectory.
layout(location = 3) out vec2 o_Velocity;
// Scene FB RT4: the diffuse half of a SKIN pixel's lighting, for the screen-space
// diffusion pass (issue #1241). This surface never shades skin, so it writes the
// "no diffusion here" code -- but it must WRITE it: an MRT output a shader leaves
// alone is undefined, not zero, and SkinDiffusion.glsl would blur the garbage
// into scene colour. See include/PBRCommon.glsl, "THE DIFFUSION HAND-OFF".
layout(location = 4) out vec4 o_SkinDiffuse;


void main()
{
    FragColor = vec4(1.0);

    vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
    vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
    o_SkinDiffuse = vec4(0.0); // not skin -- see the declaration above (#1241)
}
