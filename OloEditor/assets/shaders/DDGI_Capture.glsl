// =============================================================================
// DDGI_Capture.glsl — probe hit-point-cache capture (issue #632)
//
// Rasterizes DDGI mesh casters into one cube face cell of the 3x2 capture
// grid around a probe (DDGIProbeUpdatePass sets the per-face camera UBO +
// viewport). Writes the mini-G-buffer the resample pass converts to the
// octahedral hit cache:
//   RT0 (RGBA8)   — albedo.rgb (baseColor * texture), a = gl_FrontFacing
//                   flag (1.0 front / 0.5 back; sky texels keep the 0 clear)
//   RT1 (RGBA16F) — rg = ddgiOctEncode(worldNormal, flipped toward the viewer
//                   for backfaces), b = linear distance from the probe,
//                   a = 0 (the resample canonicalizes the flag into HitGeo.a)
//
// Culling is disabled by the pass for every caster — backface texels are the
// in-wall-probe classification signal, tagged via gl_FrontFacing here.
// All positions are render-origin-relative (issue #429): the pass shifts the
// model matrix and the probe position by the same origin.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 8 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
// pipeline has no vertex-input state, so attributes are READ from binding 57
// (the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address). The casters are MeshSource VAOs, whose stream is the
// engine `Vertex` (32 B: vec3 position @0, vec3 normal @12, vec2 uv @24),
// so the stride is 8 floats. Pulled locals in main() carry the ATTRIBUTE
// NAMES, which keeps the body identical on both routes; the GL attribute
// branch below is untouched.
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

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Per-draw capture data — mirrors DDGIPassDataUBO in DDGIProbeUpdatePass.cpp.
#include "include/DDGICommon.glsl"
#include "include/DDGIPassData.glsl"

#include "include/BindlessHeap.glsl"
// The probe's RELOCATED position is read here, on the GPU, from the probe-data
// texture (issue #707, upgrade 4). It used to arrive in u_DDGIProbePosition.xyz
// because the CPU owned the relocation offset — and that is exactly the coupling
// that forced the per-probe readback. With relocation moved into
// DDGI_Relocate.comp the CPU no longer knows the offset, so the capture derives
// the eye position itself and the CPU supplies only an eye-at-ORIGIN
// view-projection plus the probe's global index.
//
// Getting this wrong is silent: capturing from the lattice point while the
// relight stage reconstructs hit points from the relocated one offsets every
// cached hit by up to 0.45 of a cell, which reads as slightly wrong GI rather
// than as anything failing.
#ifdef OLO_BINDLESS
#define u_ProbeData OLO_HEAP_TEX_2D(1) // xyz = offsetN, w = state
#else
layout(binding = 1) uniform sampler2D u_ProbeData;
#endif

layout(location = 0) out vec3 v_ProbeRelative; // caster position RELATIVE TO THE PROBE
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

vec3 ddgiCaptureProbePosition()
{
    int probeIndex = int(u_DDGIProbePosition.w + 0.5);
    int level = ddgiCascadeOfProbeIndex(probeIndex);
    ivec3 storage = ddgiStorageCoordOfProbeIndex(probeIndex);
    vec4 pdata = texelFetch(u_ProbeData, ddgiCascadedProbeTileCoord(level, storage), 0);
    return ddgiCascadeProbeWorldPosition(storage, level, pdata.xyz);
}

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal   = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
    vec4 worldPos = u_DDGIModel * vec4(a_Position, 1.0);
    // u_ViewProjection is the face's projection times a rotation-only view (eye
    // at the ORIGIN), so the probe-relative position IS the view-space input.
    v_ProbeRelative = worldPos.xyz - ddgiCaptureProbePosition();
    v_Normal = mat3(u_DDGINormalMatrix) * a_Normal;
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * vec4(v_ProbeRelative, 1.0);
}

#type fragment
#version 460 core

#include "include/DDGICommon.glsl"
#include "include/DDGIPassData.glsl"

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). Each name maps to
// the SAME binding number the pass binds with, so the two variants cannot
// disagree; the shader BODY is byte-identical between them.
#ifdef OLO_BINDLESS
#define u_AlbedoMap OLO_HEAP_TEX_2D(0)   // white fallback when the caster has no texture
#else
layout(binding = 0) uniform sampler2D u_AlbedoMap; // white fallback when the caster has no texture
#endif

layout(location = 0) in vec3 v_ProbeRelative;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_AlbedoFlag; // rgb = albedo, a = front/back flag
layout(location = 1) out vec4 o_GeoDist;    // rg = oct normal, b = linear distance, a = 0

void main()
{
    vec3 albedo = u_DDGIBaseColor.rgb * texture(u_AlbedoMap, v_TexCoord).rgb;

    // Flip the normal toward the viewer for backfaces: single-sided geometry
    // seen from behind still reports a meaningful surface orientation, and the
    // 0.5 flag marks it as a backface hit for classification/relight.
    vec3 n = normalize(v_Normal);
    if (!gl_FrontFacing)
    {
        n = -n;
    }
    float flag = gl_FrontFacing ? DDGI_HIT_FRONTFACE : DDGI_HIT_BACKFACE;

    // The vertex stage already produced the probe-relative position, so the
    // distance costs a length() and no second probe-data fetch.
    float dist = length(v_ProbeRelative);

    o_AlbedoFlag = vec4(albedo, flag);
    o_GeoDist = vec4(ddgiOctEncode(n), dist, 0.0);
}
