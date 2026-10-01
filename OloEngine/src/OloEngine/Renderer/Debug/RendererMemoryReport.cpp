#include "OloEnginePCH.h"
#include "RendererMemoryReport.h"

namespace OloEngine
{
    MemoryReconciliation ReconcileCommittedBytes(const u64 trackedCommittedBytes,
                                                 const BackendMemoryObservation* const observation,
                                                 const bool racing)
    {
        MemoryReconciliation result;
        result.TrackedCommittedBytes = trackedCommittedBytes;
        if (!observation)
        {
            result.Status = MemoryReconciliationStatus::NoDevice;
            return result;
        }
        if (!observation->HasAllocatorTotals)
        {
            // Not "reconciled with zero difference": there was nothing to reconcile against.
            result.Status = MemoryReconciliationStatus::NotObservable;
            return result;
        }

        result.ObservedAllocationBytes = observation->AllocationBytes;
        if (racing)
        {
            // Both numbers are real, but they describe different instants. Publishing a
            // difference would invent an untracked (or double-counted) allocation.
            result.Status = MemoryReconciliationStatus::Racing;
            return result;
        }

        const auto observed = static_cast<i64>(observation->AllocationBytes);
        const auto tracked = static_cast<i64>(trackedCommittedBytes);
        result.DifferenceBytes = observed - tracked;
        if (observed == tracked)
        {
            result.Status = MemoryReconciliationStatus::Reconciled;
        }
        else if (observed > tracked)
        {
            result.Status = MemoryReconciliationStatus::Untracked;
        }
        else
        {
            result.Status = MemoryReconciliationStatus::OverCounted;
        }
        return result;
    }

    const char* ToString(const MemoryBackend backend)
    {
        switch (backend)
        {
            case MemoryBackend::OpenGL:
                return "OpenGL";
            case MemoryBackend::Vulkan:
                return "Vulkan";
            case MemoryBackend::Unknown:
                break;
        }
        return "Unknown";
    }

    const char* ToString(const MemorySizeSource source)
    {
        switch (source)
        {
            case MemorySizeSource::FormatEstimate:
                return "formatEstimate";
            case MemorySizeSource::Committed:
                return "committed";
        }
        return "unknown";
    }

    const char* ToString(const MemoryLifetime lifetime)
    {
        switch (lifetime)
        {
            case MemoryLifetime::Unattributed:
                return "unattributed";
            case MemoryLifetime::Persistent:
                return "persistent";
            case MemoryLifetime::Asset:
                return "asset";
            case MemoryLifetime::Pooled:
                return "pooled";
            case MemoryLifetime::History:
                return "history";
            case MemoryLifetime::PerFrame:
                return "perFrame";
            case MemoryLifetime::Staging:
                return "staging";
            case MemoryLifetime::PassOwned:
                return "passOwned";
            case MemoryLifetime::COUNT:
                break;
        }
        return "unknown";
    }

    const char* ToString(const MemoryReconciliationStatus status)
    {
        switch (status)
        {
            case MemoryReconciliationStatus::Reconciled:
                return "reconciled";
            case MemoryReconciliationStatus::Untracked:
                return "untracked";
            case MemoryReconciliationStatus::OverCounted:
                return "overCounted";
            case MemoryReconciliationStatus::Racing:
                return "racing";
            case MemoryReconciliationStatus::NotObservable:
                return "notObservable";
            case MemoryReconciliationStatus::NoDevice:
                return "noDevice";
        }
        return "unknown";
    }

    const char* ToString(const MemoryResidencyStatus status)
    {
        switch (status)
        {
            case MemoryResidencyStatus::OsReported:
                return "osReported";
            case MemoryResidencyStatus::AllocatorHeuristic:
                return "allocatorHeuristic";
            case MemoryResidencyStatus::Unknown:
                return "unknown";
        }
        return "unknown";
    }
} // namespace OloEngine
