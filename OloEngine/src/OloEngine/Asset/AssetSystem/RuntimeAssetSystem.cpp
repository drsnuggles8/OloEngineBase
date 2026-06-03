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
        // Reject new work and snapshot the in-flight task handles under the lock.
        TArray<Tasks::TTask<Ref<Asset>>> pending;
        {
            TUniqueLock<FMutex> lock(m_StateMutex);
            m_Running = false;
            pending.Reserve(m_InFlight.Num());
            for (const auto& load : m_InFlight)
                pending.Add(load.Task);
        }

        // Wait for every in-flight task outside the lock. Their bodies call back into
        // the owning RuntimeAssetManager, which is destroyed right after this returns,
        // so none may still be running. Tasks::TTask::Wait drives the task to
        // completion through the UE scheduler (executing it inline if it has not been
        // picked up yet), so this returns deterministically without a poll loop.
        for (auto& task : pending)
            task.Wait();

        TUniqueLock<FMutex> lock(m_StateMutex);
        m_InFlight.Reset();
    }

    void RuntimeAssetSystem::QueueAssetLoad(RuntimeAssetLoadRequest request)
    {
        if (request.Handle == 0)
        {
            OLO_CORE_ERROR("RuntimeAssetSystem: Cannot queue asset with invalid handle");
            return;
        }

        const AssetHandle handle = request.Handle;

        TUniqueLock<FMutex> lock(m_StateMutex);

        if (!m_Running)
        {
            OLO_CORE_WARN("RuntimeAssetSystem: Cannot queue asset load - system is stopped");
            return;
        }

        // Dedup: skip if this handle is already in flight.
        for (const auto& load : m_InFlight)
        {
            if (load.Handle == handle)
                return;
        }

        // Launch on the UE task scheduler. The returned TTask both tracks completion
        // and carries the loaded asset as its result, so retaining the handle is all
        // the bookkeeping the in-flight set needs.
        Tasks::TTask<Ref<Asset>> task = Tasks::Launch(
            "RuntimeAssetLoad",
            [this, handle]() -> Ref<Asset>
            {
                OLO_PROFILER_SCOPE("Runtime Asset Load Task");
                try
                {
                    return LoadAssetFromPack(handle);
                }
                catch (const std::exception& e)
                {
                    OLO_CORE_ERROR("RuntimeAssetSystem: Exception during asset loading for handle {}: {}", handle, e.what());
                }
                catch (...)
                {
                    OLO_CORE_ERROR("RuntimeAssetSystem: Unknown exception during asset loading for handle {}", handle);
                }
                return nullptr;
            },
            Tasks::ETaskPriority::BackgroundNormal);

        m_InFlight.Add(FInFlightLoad{ handle, std::move(task) });
    }

    bool RuntimeAssetSystem::RetrieveCompletedAssets(TArray<FCompletedAssetLoad>& outAssets)
    {
        OLO_PROFILER_SCOPE("RuntimeAssetSystem::RetrieveCompletedAssets");

        bool retrievedAny = false;

        TUniqueLock<FMutex> lock(m_StateMutex);

        // Walk back-to-front so RemoveAtSwap never disturbs an index we have yet to
        // visit. Only completed tasks are touched, so GetResult() returns immediately
        // (its internal wait is already satisfied) and never blocks under the lock.
        for (i32 i = m_InFlight.Num() - 1; i >= 0; --i)
        {
            FInFlightLoad& load = m_InFlight[i];
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
