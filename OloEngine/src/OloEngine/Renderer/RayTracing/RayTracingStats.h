#pragma once

// =============================================================================
// RayTracingStats.h — the acceleration-structure telemetry block. Issue #978.
//
// Subsystem-owned, in the VirtualCullStats style, rather than fields bolted
// onto Renderer3D::Statistics: an RT counter means nothing without the RT
// capability beside it, and a zero here has several distinct causes that the
// consumer has to be able to tell apart.
//
// Every counter answers one of the issue's telemetry asks. Where a zero is
// ambiguous, the comment says what the zero means — a counter whose zero has
// two readings is a counter nobody can act on.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"

#include <array>

namespace OloEngine::RayTracing
{
    // Per-frame work. Reset at the top of every scene extraction, so a zero
    // means "nothing happened this frame", never "the subsystem is off" —
    // that question is answered by Capabilities::Supported.
    struct FrameCounters
    {
        u32 BlasBuilds = 0;       ///< Full BLAS builds recorded this frame.
        u32 BlasRefits = 0;       ///< Update-mode builds (deformed geometry).
        u32 BlasCompactions = 0;  ///< Compaction copies recorded this frame.
        u32 BlasRetired = 0;      ///< BLASes handed to deferred reclaim.
        u32 TlasBuilds = 0;       ///< Full TLAS rebuilds (0 or 1 in practice).
        u32 TlasUpdates = 0;      ///< TLAS refits (0 or 1 in practice).
        u32 InstancesTraced = 0;  ///< Instances written into the TLAS build.
        u32 InstancesSkipped = 0; ///< Live GPU Scene instances that could not be traced.

        // GPU time, nanoseconds, resolved a frame or more late through
        // GPUPassTimerPool. Zero means "no sample resolved yet", which is the
        // normal state for the first few frames — not "it was free".
        u64 BlasBuildGpuNs = 0;
        u64 TlasBuildGpuNs = 0;

        // Masked-candidate accounting, only non-zero in the diagnostic trace
        // mode (the counters are GPU-side otherwise and cost a readback).
        u32 MaskedCandidatesAccepted = 0;
        u32 MaskedCandidatesRejected = 0;

        void Reset()
        {
            *this = FrameCounters{};
        }

        [[nodiscard]] auto operator==(const FrameCounters&) const -> bool = default;
    };

    // Standing totals — what currently exists, rather than what happened. Not
    // reset per frame.
    struct ResidentCounters
    {
        // BLAS population by class, counted per unique GEOMETRY — one entry
        // here is one acceleration structure. Counting it per instance instead
        // reports a mesh drawn 500 times as 500 structures, which is the
        // number a memory budget would then be sized against.
        //
        // The Unsupported row is always zero by construction: an unsupported
        // record produces no BLAS, so it is counted in UnsupportedInstances
        // below instead. The row is kept so the array stays indexable by
        // GeometryClass.
        std::array<u32, static_cast<sizet>(GeometryClass::Count)> BlasByClass{};

        u32 TlasInstances = 0; ///< Instances in the TLAS as last built.

        // Live GPU Scene instances the RT scene cannot trace, counted per
        // INSTANCE rather than per geometry — rejection is an instance-level
        // verdict (a dead material slot rejects one instance of a mesh whose
        // other instances still trace). This is the issue's
        // "unsupported/missing geometry count", and it is a real, expected
        // population: skinned, cloth, virtualized-cluster and particle
        // entities never reach the canonical GPU Scene at all.
        u32 UnsupportedInstances = 0;

        // Memory, bytes. AccelerationStructureBytes is what the BLAS/TLAS
        // backing buffers currently hold; ScratchBytes is the pooled build
        // scratch still allocated.
        u64 AccelerationStructureBytes = 0;
        u64 ScratchBytes = 0;

        // Compaction savings: how many bytes the compaction copies gave back,
        // measured as (pre-compaction size - compacted size) summed over every
        // BLAS currently resident in compacted form. Zero with a non-zero
        // BlasByClass total means nothing has compacted yet, which is normal
        // for the frame a BLAS is first built.
        u64 CompactionSavedBytes = 0;

        [[nodiscard]] u32 TotalBlas() const
        {
            u32 total = 0;
            for (const u32 count : BlasByClass)
            {
                total += count;
            }
            return total;
        }

        // Every counted BLAS is traceable — the Unsupported row is zero by
        // construction (see BlasByClass). Kept as a named accessor because
        // "how many structures can a ray actually hit" is the question
        // consumers ask, and spelling it TotalBlas() at a call site invites
        // the assumption that some of them cannot.
        [[nodiscard]] u32 TraceableBlas() const
        {
            return TotalBlas() - BlasByClass[static_cast<sizet>(GeometryClass::Unsupported)];
        }

        [[nodiscard]] auto operator==(const ResidentCounters&) const -> bool = default;
    };

    struct SceneStats
    {
        FrameCounters Frame{};
        ResidentCounters Resident{};

        // Why the TLAS was last (re)built. Meaningful only when
        // Frame.TlasBuilds or Frame.TlasUpdates is non-zero this frame;
        // otherwise it is the reason from whenever the last one happened.
        TlasBuildReason LastTlasReason = TlasBuildReason::FirstBuild;

        [[nodiscard]] auto operator==(const SceneStats&) const -> bool = default;
    };
} // namespace OloEngine::RayTracing
