#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetByteSize.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSystem/AssetLoadTicket.h"
#include "OloEngine/Asset/AssetSystem/CancellableLoadSet.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Threading/Mutex.h"

#include <functional>
#include <optional>

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
    template<>
    struct TIsTriviallyRelocatable<FCompletedAssetLoad>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(FCompletedAssetLoad::Handle)> &&
                                      TIsTriviallyRelocatable_V<decltype(FCompletedAssetLoad::LoadedAsset)>;
    };

    // One async asset load: keyed by handle, its result the loaded asset, its
    // payload the size estimate it was queued with. Task results remain in heap
    // storage; the record owns only handles.
    using FAssetLoadRecord = TCancellableLoadRecord<AssetHandle, Ref<Asset>, FAssetByteSize>;

    /**
     * @brief Counters and byte totals for the async load queue (issue #1365).
     *
     * Counters are cumulative since construction. PendingBytes sums the
     * EstimatedSize of every request still pending; its UnknownCount says how
     * many requests carried no estimate, so "unknown" is never read as zero.
     */
    struct FRuntimeAssetLoadStats
    {
        u32 PendingCount = 0;
        FAssetByteTotal PendingBytes;
        // Of PendingCount, the loads whose worker has finished and whose result waits
        // for the next RetrieveCompletedAssets().
        u32 CompletedUnretrievedCount = 0;

        // Abandoned loads whose worker has not finished yet. They still hold a
        // worker and still call back into the owning manager, so StopAndWait()
        // waits for them too.
        u32 AbandonedRunningCount = 0;

        u64 CancelledBeforeStart = 0;
        u64 AbandonedInFlight = 0;
        u64 DiscardedCompleted = 0;
        u64 AbandonedResultsDropped = 0; // abandoned loads that finished and whose result was released
        u64 RejectedWhileStopped = 0;    // QueueAssetLoad calls refused because the system was stopped
    };

    /**
     * @brief Test seam: observation and blocking points on the worker's load path.
     *
     * StartGate, when set, becomes a prerequisite of every load task launched
     * afterwards, so a test can hold a request genuinely queued (launched, not
     * started) until it triggers the gate. The hooks are copied into each task at
     * launch, so set them before queueing. OnLoadStarted runs on the worker after
     * the Queued -> Running transition and before the pack read; OnLoadFinished
     * runs after the pack read returned and before the task completes. A test
     * that blocks in either one has a load that is really in flight, with the
     * real pack read before or after it.
     *
     * What the seam does not cover: the time a real deserialize takes. The pack
     * read itself is never replaced.
     */
    struct FRuntimeAssetLoadTestHooks
    {
        std::optional<Tasks::FTaskEvent> StartGate;
        std::function<void(AssetHandle)> OnLoadStarted;
        std::function<void(AssetHandle, const Ref<Asset>&)> OnLoadFinished;
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
     * a TCancellableLoadSet guarded by a UE `FMutex`, and shutdown waits on the task
     * handles via `Tasks::TTask::Wait`. Each task's *result* is the loaded asset, so
     * the handle doubles as both the completion signal and the result channel — no
     * separate completion queue or atomic counter is needed.
     *
     * ## Cancellation contract (issue #1365)
     *
     * A result reaches the caller only through RetrieveCompletedAssets(), and only
     * for a request still live when it is retrieved. CancelAssetLoad() takes the
     * request out of the live set under m_StateMutex, the lock
     * RetrieveCompletedAssets() also takes, so a cancel and a retrieve are ordered:
     * the result is either retrieved before the cancel (which then reports
     * NotPending) or never retrieved at all. The per-state table is on
     * TCancellableLoadSet. No task is chained on a load task, so no queued work can
     * observe a dropped result.
     *
     * An abandoned task still calls back into the owning RuntimeAssetManager, so
     * StopAndWait() waits for abandoned tasks as well as live ones. A handle can
     * be queued again while its abandoned predecessor still runs; only the new
     * request's result can be retrieved.
     *
     * Records that leave the system are handed to the caller through `outDropped`,
     * so a caller holding its own lock can release the results after unlocking;
     * the overloads without it release them after m_StateMutex is dropped.
     *
     * StopAndWait() cancels every request that has not started, waits for every
     * task (live and abandoned) to finish, then drops all results. After it returns
     * no load task touches this object or its manager.
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
         * @brief Stop accepting requests, cancel the ones not yet started, and wait
         *        for every live and abandoned task to finish. All results are dropped.
         */
        void StopAndWait();

        /**
         * @brief Queue an asset for loading
         * @param request The asset load request
         * @return True if a new request was launched; false if the handle was already
         *         pending, invalid, or the system is stopped.
         */
        bool QueueAssetLoad(RuntimeAssetLoadRequest request);

        /**
         * @brief Withdraw a queued request or abandon a running one. See the
         *        cancellation contract in the class comment.
         * @param outDropped Receives a discarded record, to be released by the caller.
         */
        EAssetLoadCancelResult CancelAssetLoad(AssetHandle handle, TArray<FAssetLoadRecord>& outDropped);
        EAssetLoadCancelResult CancelAssetLoad(AssetHandle handle);

        /**
         * @brief Retrieve assets that have finished loading on worker threads
         * @param outAssets Output array, appended with completed loads.
         * @param outDropped Receives the finished abandoned loads reaped on the way,
         *        to be released by the caller.
         * @return True if any completed assets were retrieved
         *
         * Called from the main thread (by RuntimeAssetManager::SyncWithAssetThread),
         * which integrates the results into its loaded-asset cache.
         */
        bool RetrieveCompletedAssets(TArray<FCompletedAssetLoad>& outAssets, TArray<FAssetLoadRecord>& outDropped);
        bool RetrieveCompletedAssets(TArray<FCompletedAssetLoad>& outAssets);

        /**
         * @brief Check if the system is still accepting load requests
         */
        bool IsRunning() const;

        /**
         * @brief Check if an asset is currently in flight
         * @param handle The asset handle to check
         * @return True if the asset is queued or loading (a cancelled request is not)
         */
        bool IsAssetPending(AssetHandle handle) const;

        /**
         * @brief Get the number of in-flight (queued or loading) assets
         */
        sizet GetPendingAssetCount() const;

        /**
         * @brief The estimate a pending request was queued with, or std::nullopt
         *        when the handle is not pending. A pending request queued without an
         *        estimate returns an Unknown size, not std::nullopt.
         */
        std::optional<FAssetByteSize> GetPendingAssetByteSize(AssetHandle handle) const;

        FRuntimeAssetLoadStats GetStats() const;

        void SetTestHooks(FRuntimeAssetLoadTestHooks hooks);

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

        // The worker body of one started load. Takes no lock of this system's.
        Ref<Asset> RunLoad(AssetHandle handle, const std::function<void(AssetHandle)>& onStarted,
                           const std::function<void(AssetHandle, const Ref<Asset>&)>& onFinished);

      private:
        RuntimeAssetManager* m_Manager = nullptr;

        // Everything below is guarded by m_StateMutex (a UE-ported FMutex). The UE
        // task stack has no atomic wrapper of its own — its scheduler uses
        // std::atomic internally — so this system keeps its own state under the
        // mutex rather than introducing parallel atomics. The one exception is each
        // load's FAssetLoadTicket::State, which the worker reads and writes without
        // the lock; the worker never takes m_StateMutex.
        bool m_Running = true;
        TCancellableLoadSet<AssetHandle, Ref<Asset>, FAssetByteSize> m_Loads;
        u64 m_RejectedWhileStopped = 0;
        FRuntimeAssetLoadTestHooks m_TestHooks;
        mutable FMutex m_StateMutex;
    };

} // namespace OloEngine
