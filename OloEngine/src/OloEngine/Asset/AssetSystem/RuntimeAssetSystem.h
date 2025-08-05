#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Thread.h"

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_set>

namespace OloEngine
{
    /**
     * @brief Asset load request for runtime system
     */
    struct RuntimeAssetLoadRequest
    {
        AssetHandle Handle;
        
        RuntimeAssetLoadRequest() = default;
        RuntimeAssetLoadRequest(AssetHandle handle) : Handle(handle) {}
    };

    /**
     * @brief Runtime asset system for optimized async loading
     * 
     * The RuntimeAssetSystem provides optimized asset loading for shipping builds.
     * It loads assets from asset packs with minimal overhead and provides
     * efficient async loading for runtime performance.
     * 
     * Key differences from EditorAssetSystem:
     * - Simpler queue management (no file monitoring)
     * - Asset pack-based loading only
     * - Optimized for performance over flexibility
     */
    class RuntimeAssetSystem : public RefCounted
    {
    public:
        RuntimeAssetSystem();
        ~RuntimeAssetSystem();

        /**
         * @brief Stop the asset thread
         */
        void Stop();

        /**
         * @brief Stop the asset thread and wait for completion
         */
        void StopAndWait();

        /**
         * @brief Queue an asset for loading
         * @param request The asset load request
         */
        void QueueAssetLoad(const RuntimeAssetLoadRequest& request);

        /**
         * @brief Sync with the asset thread (process any completed loads)
         */
        void SyncWithAssetThread();

        /**
         * @brief Check if the asset thread is running
         * @return True if the thread is active
         */
        bool IsRunning() const { return m_Running; }

        /**
         * @brief Check if an asset is in the pending queue
         * @param handle The asset handle to check
         * @return True if the asset is pending load
         */
        bool IsAssetPending(AssetHandle handle) const;

        /**
         * @brief Get the number of pending asset loads
         * @return Number of assets in the loading queue
         */
        size_t GetPendingAssetCount() const;

    private:
        /**
         * @brief Main asset thread function
         */
        void AssetThreadFunc();

        /**
         * @brief Load an asset from the asset pack
         * @param handle The asset handle to load
         * @return Loaded asset or nullptr on failure
         */
        Ref<Asset> LoadAssetFromPack(AssetHandle handle);

    private:
        Thread m_Thread;
        std::atomic<bool> m_Running = true;

        // Asset loading queue
        std::queue<RuntimeAssetLoadRequest> m_AssetLoadingQueue;
        std::mutex m_AssetLoadingQueueMutex;
        std::condition_variable m_AssetLoadingQueueCV;

        // Completed assets (ready for main thread pickup)
        std::queue<std::pair<AssetHandle, Ref<Asset>>> m_CompletedAssets;
        std::mutex m_CompletedAssetsMutex;

        // Pending assets tracking
        std::unordered_set<AssetHandle> m_PendingAssets;
        mutable std::mutex m_PendingAssetsMutex;
    };

} // namespace OloEngine
