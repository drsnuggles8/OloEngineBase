// FoliageCullCommon.glsl — the buffer contract and the geometry the foliage GPU
// cull's two kernels share (issue #1235).
//
// C++ twin: OloEngine/src/OloEngine/Terrain/Foliage/FoliageGPUCuller.h
//           (FoliageCullLayerHeader / FoliageCullStateHeader), pinned by
//           OloEngine/tests/Rendering/CullingLOD/FoliageGPUCullParityTest.cpp,
//           which runs the real dispatch and compares its survivor set against
//           FoliageInstanceBounds() + Frustum::IsBoxVisible() on the CPU.
//
// Only the two buffers BOTH kernels touch are declared here. The instance
// stream (16), the compacted output (15) and the indirect args (17) are
// declared in FoliageInstanceCull.comp alone, so the group kernel's program
// does not carry three storage blocks it never reads.
//
// ── Why the bindings are 15..19 ──────────────────────────────────────────────
// The engine SSBO namespace is FULL (every number 0..79 is claimed;
// ShaderBindingLayout.h). These five are the instance-cull family's numbers,
// used here for the SAME roles GPUFrustumCuller uses them for — 15 compacted
// output, 16 cull input, 17 indirect args — plus its two documented
// dispatch-local slots 18/19. The foliage cull is a standalone compute
// dispatch, and every consumer of those numbers rebinds before its own use, so
// the reuse is conflict-free exactly as it is for InstanceFrustumCull.comp.
//
// ── Why the bound is rebuilt here rather than uploaded per instance ──────────
// The per-instance AABB is a pure function of the instance row (position,
// scale, height) and the LAYER's bounds profile, and that profile already
// carries the wind and interaction displacement padding the registry computed
// (#1236 / #1238). Uploading a precomputed AABB per instance would triple the
// cull's read bandwidth to restate something derivable in six multiplies, and —
// worse — would be a SECOND copy of a bound that must agree with the CPU's, so
// a sculpt that refreshed one and not the other would pop plants for a frame.

#ifndef OLO_FOLIAGE_CULL_COMMON_GLSL
#define OLO_FOLIAGE_CULL_COMMON_GLSL

// ── binding 18: per-LAYER, rebuilt only when the registry generation moves.
// Tail holds the group AABBs (8 uints per group: min.xyz, pad, max.xyz, pad —
// float bits) followed by one group index per instance row. std430 permits only
// ONE unsized array, which is why the two live in a shared tail behind an
// explicit offset rather than as two members. ────────────────────────────────
layout(std430, binding = 18) readonly buffer FoliageCullLayer
{
    uint l_GroupCount;
    uint l_InstanceCount;
    uint l_RowGroupOffset; // index into l_Tail where the row -> group table starts
    uint l_Pad0;
    // The layer's FoliageBoundsProfile — the shape ONE instance occupies, in
    // units of its own scale. Identical fields, identical meaning, identical
    // order as the C++ struct.
    float l_HalfExtentXZ;
    float l_HalfExtentXZHeightScaled;
    float l_MinY;
    float l_MaxY;
    float l_WindDisplacement;
    float l_InteractionDisplacement;
    float l_Pad1;
    float l_Pad2;
    uint l_Tail[];
};

// ── binding 19: per (layer, view). CPU seeds the header every cull; the
// kernels write the counters and the group-visibility bits. ──────────────────
layout(std430, binding = 19) buffer FoliageCullState
{
    // Six frustum planes of the view being culled FOR, in TERRAIN-LOCAL space
    // (MakeTerrainLocalCullInputs) — the same space the instance rows and the
    // group bounds live in, so the test needs no per-instance transform.
    vec4 s_Planes[6];
    // The MAIN view's position, terrain-local. NOT this view's camera: a shadow
    // cull runs under the LIGHT's frustum, but the plants it must draw are the
    // ones the beauty pass draws, and those are chosen by distance from the
    // VIEWER (FoliageParams' u_MeshViewPos carries the same position into every
    // foliage stage for exactly this reason). Culling a shadow caster by its
    // distance to the light would delete the shadows of everything the light is
    // far from.
    vec4 s_DistanceOrigin; // xyz = position, w = max draw distance (+ padding)
    uint s_InstanceCount;
    uint s_GroupCount;
    uint s_OutputCapacity;
    uint s_PartCount;
    uint s_VisibleCount;    // survivors of the per-instance test (pre-overflow)
    uint s_ReserveCursor;   // monotonic append cursor — see FoliageInstanceCull.comp
    uint s_GroupsVisible;   // survivors of the patch test
    uint s_SourceRowOffset; // index into s_Tail where the survivor -> source row map starts
    // 1 on the MAIN view's cull, 0 on every shadow view's.
    //
    // The GPUReadbackStats block is ONE per frame, so every dispatch that adds
    // to it is summed. With CSM there are five culls a frame, and a Generated
    // count summed over five views is five times the number of plants that
    // exist -- a plausible number that answers no question, which for a
    // diagnostic channel is worse than no number. So the four ratio counters
    // the issue's third criterion names describe the MAIN view alone, and the
    // overflow flag and its drop count stay unconditional because a truncation
    // in ANY view is a condition the frame has.
    uint s_EmitStats;
    uint s_StatsPad0;
    uint s_StatsPad1;
    uint s_StatsPad2;
    // Tail: group visibility (one uint per group, 1 = visible) followed by the
    // compacted slot -> SOURCE ROW map (one uint per output capacity). The
    // second is what makes "compaction preserved identity" a checkable claim
    // rather than a hope — see FoliageInstanceCull.comp.
    uint s_Tail[];
};

// Group AABB, decoded from the layer tail.
void oloFoliageGroupBounds(uint group, out vec3 lo, out vec3 hi)
{
    uint base = group * 8u;
    lo = vec3(uintBitsToFloat(l_Tail[base + 0u]),
              uintBitsToFloat(l_Tail[base + 1u]),
              uintBitsToFloat(l_Tail[base + 2u]));
    hi = vec3(uintBitsToFloat(l_Tail[base + 4u]),
              uintBitsToFloat(l_Tail[base + 5u]),
              uintBitsToFloat(l_Tail[base + 6u]));
}

// GLSL twin of OloEngine::FoliageInstanceBounds (FoliageInstanceRegistry.h),
// over the three lanes of an instance row it actually reads. Keep the two in
// step: the parity test compares survivor sets, so a drift here shows up as
// plants the CPU reference keeps and the GPU drops, not as a compile error.
void oloFoliageInstanceBounds(vec3 position, float scale, float height, out vec3 lo, out vec3 hi)
{
    float heightScale = height * scale;
    float halfXZ = max(l_HalfExtentXZ * scale, l_HalfExtentXZHeightScaled * heightScale);
    float pad = l_WindDisplacement + l_InteractionDisplacement;
    lo = position + vec3(-halfXZ, l_MinY * heightScale, -halfXZ) - vec3(pad);
    hi = position + vec3(halfXZ, l_MaxY * heightScale, halfXZ) + vec3(pad);
}

// Conservative AABB-vs-frustum test, the positive-vertex form. Twin of
// Frustum::IsBoxVisible: a box is rejected only when its FARTHEST corner along
// a plane's normal is still behind that plane.
bool oloFoliageBoxVisible(vec3 lo, vec3 hi)
{
    for (int i = 0; i < 6; ++i)
    {
        vec3 n = s_Planes[i].xyz;
        vec3 positive = vec3(n.x >= 0.0 ? hi.x : lo.x,
                             n.y >= 0.0 ? hi.y : lo.y,
                             n.z >= 0.0 ? hi.z : lo.z);
        if (dot(n, positive) + s_Planes[i].w < 0.0)
            return false;
    }
    return true;
}

// Distance from the MAIN view to the nearest point of the box. Zero inside.
float oloFoliageBoxDistance(vec3 lo, vec3 hi)
{
    vec3 origin = s_DistanceOrigin.xyz;
    vec3 closest = clamp(origin, lo, hi);
    return length(closest - origin);
}

#endif // OLO_FOLIAGE_CULL_COMMON_GLSL
