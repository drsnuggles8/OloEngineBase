#include "OloEnginePCH.h"
#include "RuntimeAssetSystem.h"

#include "OloEngine/Asset/AssetManager/RuntimeAssetManager.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <exception>
#include <utility>

namespace OloEngine
{
    const char* ToString(EAssetLoadCancelResult result) noexcept
    {
        switch (result)
        {
            case EAssetLoadCancelResult::NotPending:
                return "NotPending";
            case EAssetLoadCancelResult::CancelledBeforeStart:
                return "CancelledBeforeStart";
            case EAssetLoadCancelResult::AbandonedInFlight:
                return "AbandonedInFlight";
            case EAssetLoadCancelResult::DiscardedCompleted:
                return "DiscardedCompleted";
        }
        return "Unknown";
    }

    RuntimeAssetSystem::RuntimeAssetSystem(RuntimeAssetManager* manager)
        : m_Manager(manager)
    {
        OLO_CORE_ASSERT(manager, "RuntimeAssetSystem requires a valid owning RuntimeAssetManager");
    }

    RuntimeAssetSystem::~RuntimeAssetSystem()
    {
        StopAndWait();
    }

    void RuntimeAssetSystem::Stop()
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        m_Running = false;
    }

    void RuntimeAssetSystem::StopAndWait()
    {
        // Reject new work, withdraw every request the workers have not started, and
        // snapshot every task handle — live and abandoned — under the lock. The
        // results are dropped below regardless, so a load nobody can receive is not
        // worth running.
        TArray<Tasks::TTask<Ref<Asset>>> pending;
        {
            TUniqueLock<FMutex> lock(m_StateMutex);
            m_Running = false;
            m_Loads.CancelUnstarted();
            m_Loads.CollectTasks(pending);
        }

        // Wait for every task outside the lock. Their bodies call back into the
        // owning RuntimeAssetManager, which is destroyed right after this returns,
        // so none may still be running — an abandoned load included. Tasks::TTask::Wait
        // drives the task to completion through the UE scheduler (executing it inline
        // if it has not been picked up yet), so this returns deterministically
        // without a poll loop. A load cancelled above still runs its body, which sees
        // the Cancelled ticket and returns at once.
        for (auto& task : pending)
            task.Wait();

        // Released after the lock, like every other dropped result.
        TArray<FAssetLoadRecord> dropped;
        {
            TUniqueLock<FMutex> lock(m_StateMutex);
            m_Loads.Clear(dropped);
        }
    }

    bool RuntimeAssetSystem::QueueAssetLoad(RuntimeAssetLoadRequest request)
    {
        if (request.Handle == 0)
        {
            OLO_CORE_ERROR("RuntimeAssetSystem: Cannot queue asset with invalid handle");
            return false;
        }

        const AssetHandle handle = request.Handle;

        TUniqueLock<FMutex> lock(m_StateMutex);

        if (!m_Running)
        {
            ++m_RejectedWhileStopped;
            OLO_CORE_WARN("RuntimeAssetSystem: Cannot queue asset load for {} - system is stopped", handle);
            return false;
        }

        // Dedup: skip if this handle is already in flight. An abandoned load of the
        // same handle does not count — its result can never be retrieved.
        if (m_Loads.FindLive(handle))
            return false;

        // Launch on the UE task scheduler. The returned TTask both tracks completion
        // and carries the loaded asset as its result, so retaining the handle is all
        // the bookkeeping the in-flight set needs. The hooks are copied into the
        // task, so the worker never takes m_StateMutex.
        m_Loads.Launch(
            "RuntimeAssetLoad", handle, request.EstimatedSize,
            [this, handle, onStarted = m_TestHooks.OnLoadStarted, onFinished = m_TestHooks.OnLoadFinished]
            { return RunLoad(handle, onStarted, onFinished); },
            m_TestHooks.StartGate);
        return true;
    }

    Ref<Asset> RuntimeAssetSystem::RunLoad(AssetHandle handle, const std::function<void(AssetHandle)>& onStarted,
                                           const std::function<void(AssetHandle, const Ref<Asset>&)>& onFinished)
    {
        OLO_PROFILER_SCOPE("Runtime Asset Load Task");

        if (onStarted)
            onStarted(handle);

        Ref<Asset> asset;
        try
        {
            asset = LoadAssetFromPack(handle);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("RuntimeAssetSystem: Exception during asset loading for handle {}: {}", handle, e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("RuntimeAssetSystem: Unknown exception during asset loading for handle {}", handle);
        }

        if (onFinished)
            onFinished(handle, asset);

        return asset;
    }

    EAssetLoadCancelResult RuntimeAssetSystem::CancelAssetLoad(AssetHandle handle, TArray<FAssetLoadRecord>& outDropped)
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        return m_Loads.Cancel(handle, outDropped);
    }

    EAssetLoadCancelResult RuntimeAssetSystem::CancelAssetLoad(AssetHandle handle)
    {
        TArray<FAssetLoadRecord> dropped; // released after the lock
        return CancelAssetLoad(handle, dropped);
    }

    bool RuntimeAssetSystem::RetrieveCompletedAssets(TArray<FCompletedAssetLoad>& outAssets, TArray<FAssetLoadRecord>& outDropped)
    {
        OLO_PROFILER_SCOPE("RuntimeAssetSystem::RetrieveCompletedAssets");

        TArray<FAssetLoadRecord> completed;
        {
            TUniqueLock<FMutex> lock(m_StateMutex);
            m_Loads.ReapAbandoned(outDropped);
            m_Loads.ExtractCompleted(completed);
        }

        // Only completed tasks were extracted, so GetResult() returns immediately.
        for (FAssetLoadRecord& load : completed)
            outAssets.Add(FCompletedAssetLoad{ load.Key, load.Task.GetResult() });

        return !completed.IsEmpty();
    }

    bool RuntimeAssetSystem::RetrieveCompletedAssets(TArray<FCompletedAssetLoad>& outAssets)
    {
        TArray<FAssetLoadRecord> dropped; // released after the lock
        return RetrieveCompletedAssets(outAssets, dropped);
    }

    bool RuntimeAssetSystem::IsRunning() const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        return m_Running;
    }

    bool RuntimeAssetSystem::IsAssetPending(AssetHandle handle) const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        return m_Loads.FindLive(handle) != nullptr;
    }

    sizet RuntimeAssetSystem::GetPendingAssetCount() const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        return static_cast<sizet>(m_Loads.LiveCount());
    }

    std::optional<FAssetByteSize> RuntimeAssetSystem::GetPendingAssetByteSize(AssetHandle handle) const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        if (const FAssetLoadRecord* load = m_Loads.FindLive(handle))
            return load->Payload;
        return std::nullopt;
    }

    FRuntimeAssetLoadStats RuntimeAssetSystem::GetStats() const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);

        FRuntimeAssetLoadStats stats;
        stats.PendingCount = static_cast<u32>(m_Loads.LiveCount());
        for (const FAssetLoadRecord& load : m_Loads.Live())
            stats.PendingBytes.Add(load.Payload);
        stats.CompletedUnretrievedCount = m_Loads.CompletedLiveCount();
        stats.AbandonedRunningCount = m_Loads.AbandonedRunningCount();

        const FCancellableLoadCounters& counters = m_Loads.Counters();
        stats.CancelledBeforeStart = counters.CancelledBeforeStart;
        stats.AbandonedInFlight = counters.AbandonedInFlight;
        stats.DiscardedCompleted = counters.DiscardedCompleted;
        stats.AbandonedResultsDropped = counters.AbandonedResultsDropped;
        stats.RejectedWhileStopped = m_RejectedWhileStopped;
        return stats;
    }

    void RuntimeAssetSystem::SetTestHooks(FRuntimeAssetLoadTestHooks hooks)
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        m_TestHooks = std::move(hooks);
    }

    Ref<Asset> RuntimeAssetSystem::LoadAssetFromPack(AssetHandle handle)
    {
        OLO_PROFILER_SCOPE("RuntimeAssetSystem::LoadAssetFromPack");

        if (!m_Manager)
        {
            OLO_CORE_ERROR("RuntimeAssetSystem::LoadAssetFromPack - No owning asset manager for handle {}", handle);
            return nullptr;
        }

        // Delegate to the manager, which owns the loaded packs and performs the read +
        // deserialize under its own pack lock. Only off-thread-safe (CPU-only) types are
        // ever queued for async loading, so this performs no GPU work on the worker.
        return m_Manager->LoadAssetFromPack(handle);
    }

} // namespace OloEngine
