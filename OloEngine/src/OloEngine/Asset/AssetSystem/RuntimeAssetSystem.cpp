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
            pending.Reserve(m_InFlight.Num() + m_Abandoned.Num());
            for (const auto& load : m_InFlight)
            {
                auto expected = FAssetLoadTicket::EState::Queued;
                load.Ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Cancelled);
                pending.Add(load.Task);
            }
            for (const auto& load : m_Abandoned)
                pending.Add(load.Task);
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

        TUniqueLock<FMutex> lock(m_StateMutex);
        m_InFlight.Reset();
        m_Abandoned.Reset();
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
            ++m_Counters.RejectedWhileStopped;
            OLO_CORE_WARN("RuntimeAssetSystem: Cannot queue asset load for {} - system is stopped", handle);
            return false;
        }

        // Dedup: skip if this handle is already in flight. An abandoned load of the
        // same handle does not count — its result can never be retrieved.
        for (const auto& load : m_InFlight)
        {
            if (load.Handle == handle)
                return false;
        }

        // The hooks are copied into the task, so the worker never takes m_StateMutex.
        auto ticket = Ref<FAssetLoadTicket>::Create();
        auto body = [this, handle, ticket, onStarted = m_TestHooks.OnLoadStarted, onFinished = m_TestHooks.OnLoadFinished]() -> Ref<Asset>
        {
            return RunLoad(handle, *ticket, onStarted, onFinished);
        };

        // Launch on the UE task scheduler. The returned TTask both tracks completion
        // and carries the loaded asset as its result, so retaining the handle is all
        // the bookkeeping the in-flight set needs.
        Tasks::TTask<Ref<Asset>> task;
        if (m_TestHooks.StartGate.has_value())
        {
            task = Tasks::Launch("RuntimeAssetLoad", std::move(body), Tasks::Prerequisites(*m_TestHooks.StartGate),
                                 Tasks::ETaskPriority::BackgroundNormal);
        }
        else
        {
            task = Tasks::Launch("RuntimeAssetLoad", std::move(body), Tasks::ETaskPriority::BackgroundNormal);
        }

        m_InFlight.Add(FInFlightAssetLoad{ handle, std::move(task), std::move(ticket), request.EstimatedSize });
        return true;
    }

    Ref<Asset> RuntimeAssetSystem::RunLoad(AssetHandle handle, const FAssetLoadTicket& ticket,
                                           const std::function<void(AssetHandle)>& onStarted,
                                           const std::function<void(AssetHandle, const Ref<Asset>&)>& onFinished)
    {
        OLO_PROFILER_SCOPE("Runtime Asset Load Task");

        // The one decision point between "cancelled before start" and "running": a
        // cancel that wins this exchange guarantees the pack is never read.
        auto expected = FAssetLoadTicket::EState::Queued;
        if (!ticket.State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Running))
            return nullptr;

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

    EAssetLoadCancelResult RuntimeAssetSystem::CancelAssetLoad(AssetHandle handle)
    {
        TUniqueLock<FMutex> lock(m_StateMutex);

        for (i32 i = 0; i < m_InFlight.Num(); ++i)
        {
            FInFlightAssetLoad& load = m_InFlight[i];
            if (load.Handle != handle)
                continue;

            auto expected = FAssetLoadTicket::EState::Queued;
            if (load.Ticket->State.compare_exchange_strong(expected, FAssetLoadTicket::EState::Cancelled))
            {
                // The worker has not started and now never will read the pack. The task
                // still has to run its (now trivial) body before its handle may be
                // released safely, so it waits in m_Abandoned like a running one.
                ++m_Counters.CancelledBeforeStart;
                m_Abandoned.Add(std::move(load));
                m_InFlight.RemoveAtSwap(i);
                return EAssetLoadCancelResult::CancelledBeforeStart;
            }

            if (load.Task.IsCompleted())
            {
                // Finished but not retrieved: release the result right here.
                ++m_Counters.DiscardedCompleted;
                m_InFlight.RemoveAtSwap(i);
                return EAssetLoadCancelResult::DiscardedCompleted;
            }

            // Mid-load. The worker finishes on its own; removing the record from
            // m_InFlight is what guarantees RetrieveCompletedAssets never hands the
            // result out.
            ++m_Counters.AbandonedInFlight;
            m_Abandoned.Add(std::move(load));
            m_InFlight.RemoveAtSwap(i);
            return EAssetLoadCancelResult::AbandonedInFlight;
        }

        return EAssetLoadCancelResult::NotPending;
    }

    void RuntimeAssetSystem::ReapAbandonedLocked()
    {
        for (i32 i = m_Abandoned.Num() - 1; i >= 0; --i)
        {
            if (!m_Abandoned[i].Task.IsCompleted())
                continue;

            // A load cancelled before it started produced nothing; count only the
            // ones whose worker actually ran and whose result is released here.
            if (m_Abandoned[i].Ticket->State.load() == FAssetLoadTicket::EState::Running)
                ++m_Counters.AbandonedResultsDropped;
            m_Abandoned.RemoveAtSwap(i);
        }
    }

    bool RuntimeAssetSystem::RetrieveCompletedAssets(TArray<FCompletedAssetLoad>& outAssets)
    {
        OLO_PROFILER_SCOPE("RuntimeAssetSystem::RetrieveCompletedAssets");

        bool retrievedAny = false;

        TUniqueLock<FMutex> lock(m_StateMutex);

        ReapAbandonedLocked();

        // Walk back-to-front so RemoveAtSwap never disturbs an index we have yet to
        // visit. Only completed tasks are touched, so GetResult() returns immediately
        // (its internal wait is already satisfied) and never blocks under the lock.
        for (i32 i = m_InFlight.Num() - 1; i >= 0; --i)
        {
            FInFlightAssetLoad& load = m_InFlight[i];
            if (!load.Task.IsCompleted())
                continue;

            outAssets.Add(FCompletedAssetLoad{ load.Handle, load.Task.GetResult() });
            m_InFlight.RemoveAtSwap(i);
            retrievedAny = true;
        }

        return retrievedAny;
    }

    bool RuntimeAssetSystem::IsRunning() const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        return m_Running;
    }

    bool RuntimeAssetSystem::IsAssetPending(AssetHandle handle) const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        for (const auto& load : m_InFlight)
        {
            if (load.Handle == handle)
                return true;
        }
        return false;
    }

    sizet RuntimeAssetSystem::GetPendingAssetCount() const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        return static_cast<sizet>(m_InFlight.Num());
    }

    std::optional<FAssetByteSize> RuntimeAssetSystem::GetPendingAssetByteSize(AssetHandle handle) const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);
        for (const auto& load : m_InFlight)
        {
            if (load.Handle == handle)
                return load.EstimatedSize;
        }
        return std::nullopt;
    }

    FRuntimeAssetLoadStats RuntimeAssetSystem::GetStats() const
    {
        TUniqueLock<FMutex> lock(m_StateMutex);

        FRuntimeAssetLoadStats stats = m_Counters;
        stats.PendingCount = static_cast<u32>(m_InFlight.Num());
        stats.PendingBytes = {};
        for (const auto& load : m_InFlight)
        {
            stats.PendingBytes.Add(load.EstimatedSize);
            if (load.Task.IsCompleted())
                ++stats.CompletedUnretrievedCount;
        }

        stats.AbandonedRunningCount = 0;
        for (const auto& load : m_Abandoned)
        {
            if (!load.Task.IsCompleted())
                ++stats.AbandonedRunningCount;
        }
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
