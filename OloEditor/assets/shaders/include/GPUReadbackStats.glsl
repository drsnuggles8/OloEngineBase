#ifndef OLO_GPU_READBACK_STATS_GLSL
#define OLO_GPU_READBACK_STATS_GLSL

// =============================================================================
// GPU readback-stats channel (issue #721) -- the GLSL half.
//
// C++ twin: OloEngine/src/OloEngine/Renderer/Debug/GPUReadbackStatsRegistry.h
//           OloEngine/src/OloEngine/Renderer/Debug/GPUReadbackStats.h
// Pinned by: OloEngine/tests/Rendering/GPUReadbackStatsLayoutTest.cpp, which
//            parses THIS FILE and the registry header and compares the two
//            name -> slot maps entry by entry. Read the registry header's
//            comment before adding anything here; the short version is that the
//            contract is a set of NAMES, not a set of byte offsets, precisely so
//            that drift is detectable by text comparison.
//
// USAGE from any compute or fragment stage:
//
//     #include "../include/GPUReadbackStats.glsl"
//     ...
//     oloStatAdd(OLO_STAT_InstanceCullDrawn, 1u);
//     if (slot >= capacity)
//     {
//         oloStatOverflow(OLO_STATFLAG_InstanceCullOutput,
//                         OLO_STAT_InstanceCullDropped, 1u);
//         return;
//     }
//
// The channel is ALWAYS bound and ALWAYS allocated (144 bytes -- it is one
// four-word header plus 32 counters), so a shader never has to branch on whether
// the feature exists. `b_StatsEnabled` is the runtime gate: when it is 0 every
// helper below costs one scalar load and no atomic. That is deliberately not an
// `#ifdef` -- a preprocessor gate would fork the shader binary and make the
// instrumented build a different program from the shipped one, which is exactly
// how an instrument stops measuring the thing it was pointed at.
//
// A NOTE ON BINDING 64. The SSBO namespace under the GL 4.6 minimum of 84 is
// FULL: every number 0..83 is claimed by some namespace, and 57/63 are reserved
// engine-wide for the Vulkan vertex-pull streams. 64 is the one number never
// claimed as an SSBO -- it is TEX_DDGI_VISIBILITY in the SAMPLER namespace. On
// Vulkan's single-set model a shader that reads sampler 64 AND storage 64 is a
// real within-shader collision (issue #691, ADR item A2), so
// GPUReadbackStatsLayoutTest.NoStatsConsumerAlsoSamplesBinding64 asserts that no
// shader includes both this file and DDGICommon.glsl. If you need stats in a GI
// pass, that test is the thing that will stop you, and the fix is to renumber --
// not to delete the test.
// =============================================================================

// Reserved counter slots. MUST equal kGPUStatCounterSlots in the C++ registry:
// a std430 array length that disagrees with the allocation is a silent layout
// mismatch, not a compile error.
#define OLO_STAT_COUNTER_SLOTS 32

// ---- Counter slots (C++ twin: OLO_GPU_STAT_COUNTERS) ------------------------
const uint OLO_STAT_InstanceCullInput = 0u;
const uint OLO_STAT_InstanceCullFrustumRejected = 1u;
const uint OLO_STAT_InstanceCullOcclusionRejected = 2u;
const uint OLO_STAT_InstanceCullDrawn = 3u;
const uint OLO_STAT_InstanceCullDropped = 4u;
const uint OLO_STAT_InstanceCullPhase2Tested = 5u;
const uint OLO_STAT_InstanceCullPhase2Recovered = 6u;
const uint OLO_STAT_VSMPagesRequested = 7u;
const uint OLO_STAT_VSMPagesRequestDropped = 8u;
const uint OLO_STAT_VSMPagesAllocated = 9u;
const uint OLO_STAT_VSMPagesEvicted = 10u;
const uint OLO_STAT_VSMPagesAllocFailed = 11u;

// ---- Overflow flag bits (C++ twin: OLO_GPU_STAT_FLAGS) ----------------------
const uint OLO_STATFLAG_InstanceCullOutput = 0u;
const uint OLO_STATFLAG_VSMRequestRing = 1u;
const uint OLO_STATFLAG_VSMPhysicalPool = 2u;

// The block layout mirrors GPUReadbackStats::GPUStatsBlock (C++). Header first,
// counters last: std430 allows only the trailing member to be unsized, and
// keeping the counters a FIXED length means the block is identical in every
// shader that declares it rather than depending on include order.
layout(std430, binding = 64) buffer OloGpuReadbackStats
{
    uint b_StatsFlags;                              // atomicOr'ed overflow bitmask
    uint b_StatsEnabled;                            // 0 = every helper below is a no-op
    uint b_StatsFrameIndexLo;                       // engine frame these counters belong to
    uint b_StatsFrameIndexHi;                       //   (split: std430 has no 64-bit scalar guarantee)
    uint b_StatsCounters[OLO_STAT_COUNTER_SLOTS];
};

// Add `value` to one counter. Safe from any invocation count -- it is an atomic
// add into a scalar, so there is no work-group scan in front of it and none of
// the `if (idx >= count) return;` hang shape documented in
// docs/agent-rules/gpu-scan-compaction.md.
void oloStatAdd(uint slot, uint value)
{
    if (b_StatsEnabled != 0u)
    {
        atomicAdd(b_StatsCounters[slot], value);
    }
}

// Raise one overflow flag. Idempotent by construction (atomicOr of a bit), so a
// pass may call it from every invocation that truncated without the cost or the
// contention of a counter.
void oloStatFlag(uint flagBit)
{
    if (b_StatsEnabled != 0u)
    {
        atomicOr(b_StatsFlags, 1u << flagBit);
    }
}

// The pairing the feature exists for: raise the flag AND count how much was
// dropped. Call this on the path that refuses an append -- the flag says "this
// pass truncated", the counter says "by how much", and a consumer that sees the
// flag but a zero count knows the two disagree.
void oloStatOverflow(uint flagBit, uint droppedSlot, uint dropped)
{
    if (b_StatsEnabled != 0u)
    {
        atomicOr(b_StatsFlags, 1u << flagBit);
        atomicAdd(b_StatsCounters[droppedSlot], dropped);
    }
}

#endif // OLO_GPU_READBACK_STATS_GLSL
