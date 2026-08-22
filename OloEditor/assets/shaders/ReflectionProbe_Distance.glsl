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
    vec4 u_CaptureProbePosition; // xyz = probe world position, w unused
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

layout(location = 0) in vec3 v_WorldPos;

layout(location = 0) out vec2 o_Distance;

void main()
{
    o_Distance = vec2(length(v_WorldPos - u_CaptureProbePosition.xyz), 0.0);
}
