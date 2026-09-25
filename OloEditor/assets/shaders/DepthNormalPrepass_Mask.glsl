// =============================================================================
// DepthNormalPrepass_Mask.glsl — alpha-MASK static meshes
//
// The forward depth prepass with a normal output (issue #1452). Bound by
// CommandDispatch in place of the forward PBR programs while the forward
// prepass runs: it writes depth AND the view normal of scene attachment 2, so
// SSAO / GTAO can run between the prepass and forward colour and every forward
// shader can apply AO to its ambient term. See
// include/DepthNormalPrepassFragment.glsl for the fragment stage, and
// DepthPrepass*.glsl for the depth-only programs the DEFERRED prepass and the
// overdraw view still use.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull, the same stream PBR_MultiLight.glsl
// reads (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24).
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

#include "include/CameraCommon.glsl"
#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

// ROUTE PARITY with the colour pass (DepthPrepass.glsl's note): the samplers
// below put this program on the same raw-GLSL route PBR_MultiLight takes, which
// `invariant gl_Position` needs — it is a promise between two programs, and it
// only holds within one compiler front-end.
invariant gl_Position;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
    OLO_INSTANCE_FORWARD();
    // The colour pass's exact association: world position first, then
    // view-projection — the colour pass re-tests this depth at GL_LEQUAL.
    v_WorldPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(u_Normal) * a_Normal;
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * vec4(v_WorldPos, 1.0);
}

#type fragment
#version 460 core

// The Vulkan material-heap arm's directives, which GLSL requires before any
// other token and which therefore cannot live in the shared include.
#ifdef OLO_VULKAN
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
#define OLO_MATERIAL_VULKAN_HEAP_READER 1
#endif

#define OLO_DEPTH_NORMAL_PREPASS_MASK 1
#include "include/DepthNormalPrepassFragment.glsl"
