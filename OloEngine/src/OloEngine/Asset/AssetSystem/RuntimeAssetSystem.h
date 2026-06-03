#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Threading/Mutex.h"

namespace OloEngine
{
    class RuntimeAssetManager;

    /**
     * @brief An async-loaded asset retrieved from the worker pool, ready for the main
     *        thread to integrate. A null LoadedAsset means that handle failed to load.
     */
    struct FCompletedAssetLoad
    {
        AssetHandle Handle = 0;
        Ref<Asset> LoadedAsset; // named LoadedAsset (not Asset) so it doesn't shadow the Asset type
    };

    /**
     * @brief Runtime asset system for optimized async loading
     *
     * The RuntimeAssetSystem provides optimized asset loading for shipping builds.
     * It loads assets from asset packs with minimal overhead and provides
     * efficient async loading for runtime performance.
     *
     * Concurrency is built entirely on the UE-ported task/threading stack: loads run
     * as `Tasks::TTask` jobs on `LowLevelTasks::FScheduler`, in-flight bookkeeping is
     * a `TArray` guarded by a UE `FMutex`, and shutdown waits on the task handles via
     * `Tasks::TTask::Wait`. Each task's *result* is the loaded asset, so the handle
     * doubles as both the completion signal and the result channel — no separate
     * completion queue or atomic counter is needed.
     *
     * Key differences from EditorAssetSystem:
     * - Simpler bookkeeping (no file monitoring)
     * - Asset pack-based loading only
     * - Optimized for performance over flexibility
     */
    class RuntimeAssetSystem : public RefCounted
    {
      public:
        /**
         * @param manager Owning RuntimeAssetManager. The system delegates the actual
         *        pack read/deserialize back to it (single source of truth for loaded
         *        packs) and never outlives it — StopAndWait() drains in-flight tasks
         *        before the manager is destroyed.
         */
        explicit RuntimeAssetSystem(RuntimeAssetManager* manager);
        ~RuntimeAssetSystem();

        // Delete copy and move operations
        RuntimeAssetSystem(const RuntimeAssetSystem&) = delete;
        RuntimeAssetSystem& operator=(const RuntimeAssetSystem&) = delete;
        RuntimeAssetSystem(RuntimeAssetSystem&&) = delete;
        RuntimeAssetSystem& operator=(RuntimeAssetSystem&&) = delete;

        /**
         * @brief Stop accepting new load requests
         */
        void Stop();

        /**
         * @brief Stop accepting new requests and wait for in-flight loads to finish
         */
        void StopAndWait();

        /**
         * @brief Queue an asset for loading
         * @param request The asset load request
         */
        void QueueAssetLoad(RuntimeAssetLoadRequest request);

        /**
         * @brief Retrieve assets that have finished loading on worker threads
         * @param outAssets Output array, appended with completed loads.
         * @return True if any completed assets were retrieved
         *
         * Called from the main thread (by RuntimeAssetManager::SyncWithAssetThread),
         * which integrates the results into its loaded-asset cache.
         */
        bool RetrieveCompletedAssets(TArray<FCompletedAssetLoad>& outAssets);

        /**
         * @brief Check if the system is still accepting load requests
         */
        bool IsRunning() const;

        /**
         * @brief Check if an asset is currently in flight
         * @param handle The asset handle to check
         * @return True if the asset is queued or loading
         */
        bool IsAssetPending(AssetHandle handle) const;

        /**
         * @brief Get the number of in-flight (queued or loading) assets
         */
        sizet GetPendingAssetCount() const;

      private:
        /**
         * @brief Load an asset from the asset pack (runs on a worker thread)
         * @param handle The asset handle to load
         * @return Loaded asset or nullptr on failure
         *
         * Delegates to the owning RuntimeAssetManager, which holds the loaded packs.
         * Only types whose serializer reports CanDeserializeFromAssetPackOffThread()
         * are queued here, so this never touches GPU resources off the main thread.
         */
        Ref<Asset> LoadAssetFromPack(AssetHandle handle);

      private:
        /// One in-flight load. The TTask both signals completion (IsCompleted) and
        /// carries the loaded asset as its result (GetResult).
        struct FInFlightLoad
        {
            AssetHandle Handle = 0;
            Tasks::TTask<Ref<Asset>> Task;
        };

        RuntimeAssetManager* m_Manager = nullptr;

        // m_Running and m_InFlight are both guarded by m_StateMutex (a UE-ported
        // FMutex). The UE task stack has no atomic wrapper of its own — its scheduler
        // uses std::atomic internally — so this system keeps its own state under the
        // mutex rather than introducing parallel atomics.
        bool m_Running = true;
        TArray<FInFlightLoad> m_InFlight;
        mutable FMutex m_StateMutex;
    };

} // namespace OloEngine
