#ifndef TERRAIN_PICK_COMMON_GLSL
#define TERRAIN_PICK_COMMON_GLSL

// =============================================================================
// GPU terrain picking — the shared state block and the two primitives the three
// pick kernels agree on (issue #717).
//
// Picking is a RAY-GUIDED variant of the LOD descent in TerrainNodeSelect.comp:
// same node pyramid, same packed coords, same worklist shape, same all-four-
// children-or-none reservation. What changes is the test — a ray/AABB slab test
// instead of a frustum test — and what a selected node means: a leaf the ray
// passes through is a CANDIDATE the resolve kernel marches, not a patch to draw.
//
// Everything the pass needs lives in ONE buffer at SSBO binding 58. That is not
// tidiness: the buffer-binding namespace is full (see
// docs/agent-rules/gpu-readback-stats-channel.md §8), so a new pass either rides
// an existing block or renumbers a family. Riding the terrain family works
// because the pick kernels never run in the same dispatch as the cull kernels
// and every producer rebinds the slots it uses — the LOD descent rebinds 58/65/
// 66/67 at the top of every TerrainGPUQuadtree::Dispatch, and the terrain draw
// carries its own visible-node handle in the draw packet.
//
// C++ twin: TerrainGPUPicker::PickStateHeader in
// OloEngine/src/OloEngine/Terrain/TerrainGPUPicker.h. The byte offsets of
// DescentDispatch/ResolveDispatch are passed straight to
// DispatchComputeIndirect, and the 16 bytes at offset 48 are the ONLY range
// copied into the readback ring — both are part of the contract, not layout
// details. Pinned by TerrainGPUPickerTest.
// =============================================================================

#include "TerrainQuadtreeCommon.glsl"

layout(std430, binding = 58) buffer TerrainPickState
{
    // ---- worklist counters + indirect arguments ------------------------------
    uint PendingCount;     //   0 — nodes this level's descent must process
    uint NextCount;        //   4 — children appended for the next level
    uint CandidateCount;   //   8 — leaf nodes the ray passes through
    uint OverflowFlags;    //  12 — see OLO_TERRAIN_PICK_OVERFLOW_* below
    uvec3 DescentDispatch; //  16 — next level's glDispatchComputeIndirect args
    uint _pickPad0;        //  28
    uvec3 ResolveDispatch; //  32 — one work group per candidate
    uint _pickPad1;        //  44

    // ---- the result, and the ONLY range the readback ring copies -------------
    // HitTBits is the ray parameter t of the nearest hit, as
    // floatBitsToUint(t). That encoding is monotonic for t >= 0, which is what
    // lets atomicMin pick the NEAREST hit across every candidate with no
    // ordering and no second pass. 0xFFFFFFFF (the reset value) means "no hit"
    // — it is above every finite positive float's bit pattern, so a real hit
    // always wins the min.
    uint HitTBits;      //  48
    uint ResultFlags;   //  52 — OverflowFlags, republished so the CPU learns
                        //       about a truncated worklist through the ring
                        //       instead of through a GetData() stall.
    uint RayId;         //  56 — echoed straight back; tells a consumer WHICH
                        //       ray this answer belongs to.
    uint _pickPad2;     //  60

    // ---- CPU-written parameters ---------------------------------------------
    // Terrain-LOCAL, exactly like the cull inputs: node bounds are terrain-local
    // and evaluating there keeps the math precise far from the world origin.
    vec4 RayOriginAndMaxDist;  //  64 — xyz = origin, w = max distance
    vec4 RayDirAndInflate;     //  80 — xyz = normalized direction, w = AABB inflation
    vec4 TerrainSizeAndScale;  //  96 — x/y = world size X/Z, z = height scale, w = heightmap texel size in world units
    uvec4 PickParams;          // 112 — x = max depth, y = node-list capacity, z = candidate capacity, w = heightmap resolution

    // ---- the candidate list -------------------------------------------------
    uint Candidates[];         // 128 — packed node coords, appended by the descent
} b_Pick;

#define OLO_TERRAIN_PICK_OVERFLOW_NODES      1u
#define OLO_TERRAIN_PICK_OVERFLOW_CANDIDATES 2u
// The resolve kernel's per-lane sample budget was not enough to step the marched
// segment at heightmap-texel spacing, so the march may have stepped OVER a thin
// above->below crossing. Reported rather than left silent, because the symptom
// otherwise is a no-hit that is indistinguishable from the ray genuinely missing
// — and "within a texel" is this feature's acceptance criterion, so a march that
// could not honour it has to say so.
#define OLO_TERRAIN_PICK_OVERFLOW_MARCH      4u

// The HitTBits reset value: the sentinel that means "nothing was hit". Twin of
// TerrainGPUPicker::kNoHitBits.
#define OLO_TERRAIN_PICK_NO_HIT 0xFFFFFFFFu

// ---- Node AABB ---------------------------------------------------------------
// The same terrain-local box TerrainNodeSelect.comp builds: XZ from the node's
// share of the footprint, Y from the precomputed min/max pyramid. `inflate`
// widens it on every axis to absorb precision error — a ray that grazes a node
// boundary must not be culled out of a leaf whose heightmap it really does
// touch, and the cost of an over-inclusive candidate is one extra marched
// segment, never a wrong answer (the resolve kernel takes the nearest hit
// across every candidate, so a spurious candidate contributes nothing).
void oloTerrainPickNodeBounds(uint level, uint nx, uint ny, vec2 heightRange, float inflate,
                              out vec3 boundsMin, out vec3 boundsMax)
{
    float span = 1.0 / float(1u << level);
    float minX = float(nx) * span * b_Pick.TerrainSizeAndScale.x;
    float maxX = float(nx + 1u) * span * b_Pick.TerrainSizeAndScale.x;
    float minZ = float(ny) * span * b_Pick.TerrainSizeAndScale.y;
    float maxZ = float(ny + 1u) * span * b_Pick.TerrainSizeAndScale.y;

    boundsMin = vec3(minX, heightRange.x, minZ) - vec3(inflate);
    boundsMax = vec3(maxX, heightRange.y, maxZ) + vec3(inflate);
}

// ---- Ray / AABB slab test ----------------------------------------------------
// Returns the overlap of the ray's [tMin, tMax] window with the box, clipped to
// the caller's window. False when the box is missed.
//
// Division by a zero direction component is deliberate rather than branched
// around: IEEE gives +/-infinity, the two slab bounds come out +inf and -inf in
// the correct order after the min/max, and an axis-parallel ray therefore falls
// out as "this axis does not constrain t" — which is the right answer. A NaN
// would break that, which is why the CPU refuses to submit a non-finite ray.
bool oloTerrainPickRayAABB(vec3 origin, vec3 dir, vec3 boundsMin, vec3 boundsMax,
                           inout float tMin, inout float tMax)
{
    vec3 invDir = vec3(1.0) / dir;
    vec3 t0 = (boundsMin - origin) * invDir;
    vec3 t1 = (boundsMax - origin) * invDir;
    vec3 tNear = min(t0, t1);
    vec3 tFar = max(t0, t1);

    tMin = max(tMin, max(tNear.x, max(tNear.y, tNear.z)));
    tMax = min(tMax, min(tFar.x, min(tFar.y, tFar.z)));
    return tMax >= tMin;
}

#endif // TERRAIN_PICK_COMMON_GLSL
