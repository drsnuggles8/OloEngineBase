// =============================================================================
// PrefixSum.glsl — work-group exclusive prefix sum (parallel scan), issue #713.
//
// The primitive every GPU compaction wants and the engine did not have. Before
// this, every compaction pass allocated its output slots with a global
// `atomicAdd` — `LightCulling.comp`, `Particle_Compact.comp`,
// `Fluid_Compact.comp`, `InstanceFrustumCull.comp`, `VirtualClusterCull.comp`.
// That works, but the slot a survivor receives is decided by whichever
// invocation happens to reach the atomic first, so the compacted output is in
// an arbitrary, frame-to-frame-varying order. A scan replaces the race with
// arithmetic: survivor `i` lands at the count of survivors before it, which is
// a pure function of the input. Same set, defined order.
//
//   layout(local_size_x = 256) in;             // BEFORE the include — see contract 2
//   #define OLO_PREFIX_SUM_GROUP_SIZE 256      // BEFORE the include
//   #include "../include/PrefixSum.glsl"
//   ...
//   uint total;
//   uint slot = OloPrefixSumExclusive(keep ? 1u : 0u, total);
//
// Reference implementation, Apache-2.0, adapted rather than copied:
// Timberdoodle's `src/rendering/tasks/prefix_sum.glsl` (Ipotrick). What is
// taken from it is the shape — subgroup scan, per-subgroup totals through
// shared memory, a device level built from per-block totals. What is not: this
// file produces an EXCLUSIVE scan (Timberdoodle's is inclusive), detects the
// subgroup extension instead of assuming it, and does not assume a 32-wide
// subgroup — see WHY THE SUBGROUP TOTALS ARE SCANNED THE SLOW WAY below.
//
// -----------------------------------------------------------------------------
// THE CONTRACT — GET THIS WRONG AND THE SHADER HANGS.
//
// 1. EVERY invocation of the work group must call `OloPrefixSumExclusive`, the
//    same number of times, in the same order. It contains `barrier()`, and a
//    barrier that only some invocations reach is undefined behaviour (in
//    practice: a GPU hang or a silently wrong scan). So the usual compute-shader
//    opening
//
//        if (idx >= u_Count) return;      // <-- WRONG before a scan
//
//    must become
//
//        uint value = (idx < u_Count) ? 1u : 0u;   // out-of-range contributes 0
//        uint slot = OloPrefixSumExclusive(value, total);
//        if (idx < u_Count) { ...use slot... }
//
//    and any loop around the scan must have a work-group-UNIFORM trip count
//    (round the bound up to a multiple of the work-group size).
//
// 2. `OLO_PREFIX_SUM_GROUP_SIZE` must equal the shader's total work-group size
//    (`local_size_x * local_size_y * local_size_z`), be a power of two, and be
//    at most 1024. Checked at compile time below where the language allows it.
//
//    AND the `layout(local_size_*) in;` declaration must come BEFORE this
//    `#include`, not after it. The subgroup path reads `gl_NumSubgroups` /
//    `gl_SubgroupID`, which the driver derives from `gl_WorkGroupSize`, and
//    NVIDIA's GLSL compiler rejects a use of that builtin that precedes the
//    layout qualifier declaring it:
//
//        error C7594: OpenGL requires declaring a layout qualifier for work
//                     group size before using the builtin constant gl_WorkGroupSize
//
//    glslang/`glslc` accept either order, so this is a failure only the real
//    driver reports — put the layout first and it cannot arise on either.
//
// 3. The scan indexes by `gl_LocalInvocationIndex`, so a 2D/3D work group works
//    unchanged (`LightCulling.comp` is 16x16).
//
// 4. One shared scratch array is declared here, so a shader gets ONE scan
//    instance. Calling it repeatedly is fine (each call ends with a barrier, so
//    the next call may overwrite the scratch immediately); calling two
//    independent scans concurrently is not.
//
// -----------------------------------------------------------------------------
// HOW TO TAKE THE SUBGROUP PATH — AND WHY IT IS NOT AUTO-DETECTED.
//
// Opting in takes THREE lines in the including shader, in this order:
//
//     #version 460 core
//     #extension GL_KHR_shader_subgroup_basic      : require
//     #extension GL_KHR_shader_subgroup_arithmetic : require
//     #define OLO_PREFIX_SUM_USE_SUBGROUP 1
//     #define OLO_PREFIX_SUM_GROUP_SIZE 256
//     #include "../include/PrefixSum.glsl"
//
// The `#extension` directives cannot live in THIS file: GLSL requires every one
// of them to precede all non-preprocessor tokens, so putting them here would
// force every consumer to place its `#include` above its first declaration — a
// per-file rule that is invisible until a driver rejects it. That is the same
// constraint `BindlessHeap.glsl` documents for `GL_ARB_bindless_texture`.
//
// THE SEPARATE `#define` IS NOT REDUNDANT, and this is the trap. The obvious
// design is to skip it and auto-detect with `#ifdef
// GL_KHR_shader_subgroup_arithmetic`, since GLSL predefines an extension's
// macro when it is enabled. **glslang predefines it either way** — it advertises
// what the COMPILER knows, not what the shader has ENABLED. So the auto-detect
// selects the subgroup path in every shader, including the ones that never
// enabled the extension, and they fail to compile outright:
//
//     error: 'subgroupExclusiveAdd' : required extension not requested:
//            GL_KHR_shader_subgroup_arithmetic
//     error: 'subgroup op' : requires SPIR-V 1.3
//
// (Both routes reject it — `--target-env=vulkan1.2` on the first line,
// `--target-env=opengl4.5` on both.) An explicit opt-in cannot drift from the
// `#extension` lines the way a detected one can.
//
// Both paths compute the identical exclusive scan, so opting in changes speed,
// never results. The engine's PRODUCTION `.comp` files deliberately do NOT opt
// in: they are also compiled headless by `ShaderHarness::CompileStageToSpv`
// under `shaderc_target_env_opengl` / OpenGL 4.5, whose SPIR-V 1.0 output
// cannot represent subgroup arithmetic at all. The portable path is the one
// provably compiled by every route the engine has.
// =============================================================================

#ifndef OLO_PREFIX_SUM_GLSL
#define OLO_PREFIX_SUM_GLSL

#ifndef OLO_PREFIX_SUM_GROUP_SIZE
#error "PrefixSum.glsl: #define OLO_PREFIX_SUM_GROUP_SIZE (== the work-group size) before including."
#endif

#if (OLO_PREFIX_SUM_GROUP_SIZE & (OLO_PREFIX_SUM_GROUP_SIZE - 1)) != 0
#error "PrefixSum.glsl: OLO_PREFIX_SUM_GROUP_SIZE must be a power of two."
#endif

#if OLO_PREFIX_SUM_GROUP_SIZE > 1024
#error "PrefixSum.glsl: OLO_PREFIX_SUM_GROUP_SIZE must be <= 1024 (GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS minimum)."
#endif

// Explicit, never detected — see the block above for why `#ifdef
// GL_KHR_shader_subgroup_arithmetic` is not a support test.
#if defined(OLO_PREFIX_SUM_USE_SUBGROUP) && (OLO_PREFIX_SUM_USE_SUBGROUP != 0)
#define OLO_PREFIX_SUM_SUBGROUP 1
#else
#define OLO_PREFIX_SUM_SUBGROUP 0
#endif

// `barrier()` alone is an execution barrier. The GLSL spec's shared-memory
// visibility guarantee for compute shaders is worded loosely enough that every
// reference implementation pairs it with `memoryBarrierShared()`; so does the
// Timberdoodle original, and so does this.
#define OLO_PREFIX_SUM_BARRIER() { memoryBarrierShared(); barrier(); }

// Scratch. Sized for the widest use of either path:
//   * shared path — one slot per invocation;
//   * subgroup path — one slot per subgroup, and `gl_NumSubgroups` is bounded
//     above by the work-group size (a hypothetical 1-wide subgroup).
shared uint g_OloPrefixSumScratch[OLO_PREFIX_SUM_GROUP_SIZE];

// Hillis-Steele inclusive scan of `g_OloPrefixSumScratch[0 .. count)`, in place.
// `count` MUST be work-group uniform. Every invocation must call it; the ones
// with `gl_LocalInvocationIndex >= count` participate in the barriers only.
//
// O(n log n) work rather than Blelloch's O(n), which is the right trade at these
// sizes: log2(256) = 8 barrier-separated steps with no bank-conflict padding and
// no two-phase up/down sweep, against a work saving that never shows up because
// the work group is latency-bound, not ALU-bound.
void OloPrefixSumScratchInclusive(uint count)
{
    uint lane = gl_LocalInvocationIndex;

    for (uint offset = 1u; offset < count; offset <<= 1)
    {
        // Read the whole step's inputs, THEN barrier, THEN write: without the
        // split, an invocation could overwrite a neighbour's source before that
        // neighbour has read it.
        uint addend = 0u;
        if (lane < count && lane >= offset)
            addend = g_OloPrefixSumScratch[lane - offset];

        // Both barriers are outside the `if` on purpose — a barrier in
        // non-uniform control flow is undefined behaviour.
        OLO_PREFIX_SUM_BARRIER();

        if (lane < count)
            g_OloPrefixSumScratch[lane] += addend;

        OLO_PREFIX_SUM_BARRIER();
    }
}

// Work-group-wide EXCLUSIVE prefix sum over `value`.
//
// Returns the sum of every lower-indexed invocation's `value` (0 for
// `gl_LocalInvocationIndex == 0`), and writes the work group's grand total to
// `groupTotal` — the same value in every invocation, which is what a caller
// needs to allocate its output range with a single atomic.
//
// Read the CONTRACT block at the top of this file before calling.
uint OloPrefixSumExclusive(uint value, out uint groupTotal)
{
#if OLO_PREFIX_SUM_SUBGROUP
    // ── Subgroup path ──
    // `subgroupExclusiveAdd` gives the prefix within this subgroup for free;
    // only the per-subgroup totals have to travel through shared memory.
    uint laneExclusive = subgroupExclusiveAdd(value);
    uint subgroupTotal = subgroupAdd(value);

    // WHY `subgroupElect()` AND `subgroupAdd` RATHER THAN "the last lane writes
    // its inclusive value": that idiom assumes lane `gl_SubgroupSize - 1` is
    // active and holds the largest prefix, which is only true for a full,
    // in-order subgroup. These two intrinsics are defined over the active set,
    // so the code stays correct if it is ever called with a partially active
    // subgroup — and costs nothing when it is not.
    if (subgroupElect())
        g_OloPrefixSumScratch[gl_SubgroupID] = subgroupTotal;

    OLO_PREFIX_SUM_BARRIER();

    // WHY THE SUBGROUP TOTALS ARE SCANNED THE SLOW WAY.
    // The Timberdoodle original scans them with a second subgroup op, which is
    // correct only while `numSubgroups <= subgroupSize` — true for its fixed
    // 1024-wide group on 32-wide hardware, not true in general (`gl_SubgroupSize`
    // is 8 on some Intel parts, and is allowed to vary per dispatch). Scanning
    // them in shared memory instead is a handful of extra barriers over at most
    // `groupSize / subgroupSize` entries and removes the assumption entirely.
    // `gl_NumSubgroups` is work-group uniform, so the trip count is uniform.
    OloPrefixSumScratchInclusive(gl_NumSubgroups);

    uint subgroupOffset = (gl_SubgroupID == 0u) ? 0u : g_OloPrefixSumScratch[gl_SubgroupID - 1u];
    groupTotal = g_OloPrefixSumScratch[gl_NumSubgroups - 1u];

    // Leave the scratch free for the next call.
    OLO_PREFIX_SUM_BARRIER();

    return subgroupOffset + laneExclusive;
#else
    // ── Portable shared-memory path ──
    uint lane = gl_LocalInvocationIndex;

    g_OloPrefixSumScratch[lane] = value;
    OLO_PREFIX_SUM_BARRIER();

    OloPrefixSumScratchInclusive(uint(OLO_PREFIX_SUM_GROUP_SIZE));

    uint inclusive = g_OloPrefixSumScratch[lane];
    groupTotal = g_OloPrefixSumScratch[uint(OLO_PREFIX_SUM_GROUP_SIZE) - 1u];

    // Leave the scratch free for the next call.
    OLO_PREFIX_SUM_BARRIER();

    // Inclusive minus own contribution is the exclusive prefix. Unsigned
    // subtraction is exact here — `inclusive >= value` always holds.
    return inclusive - value;
#endif
}

// Convenience overload for callers that do not need the grand total.
uint OloPrefixSumExclusive(uint value)
{
    uint ignored;
    return OloPrefixSumExclusive(value, ignored);
}

#endif // OLO_PREFIX_SUM_GLSL
