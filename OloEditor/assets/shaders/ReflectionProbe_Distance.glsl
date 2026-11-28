// =============================================================================
// ReflectionProbe_Distance.glsl — radial-distance capture for distance-impostor
// reflection probes (issue #705).
//
// Rasterizes the scene's opaque mesh casters into one face of an RG32F
// cube-face target around the probe being baked
// (ReflectionProbeBaker::CaptureDistanceField drives the 6-face loop, sets
// the per-face camera UBO and the per-draw model UBO). Writes:
//   RT0.r — linear radial distance from the probe centre, world units.
//   RT0.g — unused (0), reserved.
// Texels no caster covers keep the clear value kProbeDistanceFar, which IS
// the miss sentinel of the encoding contract in ReflectionProbeDistanceField.h.
//
// PEELING (issue #1804): each face is captured three times. With
// u_CaptureProbePosition.w = 0 the pass keeps the nearest surface (d0). With
// w = 1 it discards every fragment not strictly beyond the previous layer,
// read texel-for-texel from u_PeelSource (TEX_USER_0, the previous pass's
// RT0 on the same-sized target), so the depth test keeps the next surface
// out: d1 against d0, then d2 against d1. The epsilon mirrors
// kProbePeelEpsilonAbs/Rel; a previous layer at the sentinel discards
// everything, so the next layer stays at the sentinel too.
//
// Depth-tests with a regular depth buffer: along one pixel's view ray, depth
// order equals radial order, so the closest surface wins. Culling is disabled
// by the pass so probe-behind-geometry reads backface distances rather than
// seeing through walls.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
// pipeline has no vertex-input state, so attributes are READ from binding 57
// (the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address). The casters are MeshSource VAOs, whose stream is the
// engine `Vertex` (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24), so
// the stride is 8 floats even though this stage only needs the position.
// The GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Per-draw capture data — mirrors ProbeDistanceCaptureUBO in
// ReflectionProbeBaker.cpp.
layout(std140, binding = 7) uniform ProbeDistanceCaptureData
{
    mat4 u_CaptureModel;         // caster world transform
    vec4 u_CaptureProbePosition; // xyz = probe world position, w = 1 to peel against u_PeelSource
};

layout(location = 0) out vec3 v_WorldPos;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    vec4 worldPos = u_CaptureModel * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 460 core

layout(std140, binding = 7) uniform ProbeDistanceCaptureData
{
    mat4 u_CaptureModel;
    vec4 u_CaptureProbePosition;
};

#include "include/BindlessHeap.glsl"

// The previous peel layer (TEX_USER_0). Heap-bindless conversion (issue #691):
// only the declaration moves between the variants.
#ifdef OLO_BINDLESS
#define u_PeelSource OLO_HEAP_TEX_2D(10)
#else
layout(binding = 10) uniform sampler2D u_PeelSource;
#endif

layout(location = 0) in vec3 v_WorldPos;

layout(location = 0) out vec2 o_Distance;

#define OLO_PROBE_PEEL_EPSILON_ABS 0.01
#define OLO_PROBE_PEEL_EPSILON_REL 0.002

void main()
{
    float radial = length(v_WorldPos - u_CaptureProbePosition.xyz);
    if (u_CaptureProbePosition.w > 0.5)
    {
        float previous = texelFetch(u_PeelSource, ivec2(gl_FragCoord.xy), 0).r;
        if (radial <= previous + max(OLO_PROBE_PEEL_EPSILON_ABS, previous * OLO_PROBE_PEEL_EPSILON_REL))
        {
            discard;
        }
    }
    o_Distance = vec2(radial, 0.0);
}
