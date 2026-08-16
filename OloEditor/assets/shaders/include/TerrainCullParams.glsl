#ifndef TERRAIN_CULL_PARAMS_GLSL
#define TERRAIN_CULL_PARAMS_GLSL

// =============================================================================
// The GPU terrain quadtree's shared parameter block and state buffer (#714).
//
// Included by the four cull kernels only — NOT by the terrain vertex stages,
// which would otherwise declare an unused UBO at binding 79 and drag it into
// every terrain pipeline layout. C++ twin: UBOStructures::TerrainCullUBO
// (ShaderBindingLayout::UBO_TERRAIN_CULL).
//
// Everything the descent needs is precomputed on the CPU so the GPU test is
// expression-for-expression the same as TerrainQuadtree::SelectNode's — the
// frustum planes come from Frustum::Update() and `projScale` from
// CalculateScreenSpaceError's `viewProjection[1][1] * viewportHeight * 0.5`.
// That is what makes TerrainGPUQuadtreeParityTest able to assert an identical
// selected-node set instead of a fuzzy one.
// =============================================================================

layout(std140, binding = 79) uniform TerrainCullParams
{
    // 0 — six frustum planes in TERRAIN-LOCAL space (xyz = normal, w = distance),
    // normalized, ordered Near, Far, Left, Right, Top, Bottom exactly as
    // Frustum::Planes. The test below is the positive-vertex form of
    // Frustum::IsBoxVisible.
    vec4 u_TerrainFrustumPlanes[6];
    // 96 — xyz = terrain-local camera position, w = projection scale
    // (viewProjection[1][1] * viewportHeight * 0.5).
    vec4 u_TerrainCameraAndProjScale;
    // 112 — x/y = terrain world size X/Z, z = target screen-space triangle size
    // (the split threshold), w = unused.
    vec4 u_TerrainSizeAndTarget;
    // 128 — x = max quadtree depth, y = visible-node capacity,
    // z = patch grid resolution K, w = max seam delta (log2 K).
    uvec4 u_TerrainLevelParams;
    // 144 — x = per-pass node-list capacity, y = LOD map resolution (1 << maxDepth),
    // z = total node count in the pyramid, w = patch mesh index count.
    uvec4 u_TerrainBufferParams;
};

// The persistent worklist state. Also the source of the two indirect argument
// blocks: SelectDispatch drives the next level's glDispatchComputeIndirect,
// SeamDispatch the per-visible-node seam kernel. Byte offsets are load-bearing
// (C++ passes them to DispatchComputeIndirect) — see TerrainGpuCullState.
layout(std430, binding = 58) buffer TerrainCullState
{
    uint PendingCount;   // 0  — nodes this level's dispatch must process
    uint NextCount;      // 4  — children appended for the next level
    uint VisibleCount;   // 8  — nodes selected for rendering so far
    uint OverflowFlags;  // 12 — bit0 = node list overflowed, bit1 = visible list overflowed
    uvec3 SelectDispatch; // 16
    uint _statePad0;      // 28
    uvec3 SeamDispatch;   // 32
    uint _statePad1;      // 44
} b_TerrainState;

#define OLO_TERRAIN_OVERFLOW_NODES   1u
#define OLO_TERRAIN_OVERFLOW_VISIBLE 2u

#endif // TERRAIN_CULL_PARAMS_GLSL
