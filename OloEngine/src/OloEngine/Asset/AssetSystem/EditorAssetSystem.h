#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Core/Thread.h"

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    /**
     * @brief Response structure for editor asset loading
     */
    struct EditorAssetLoadResponse
    {
        AssetMetadata RequestMetadata;
        Ref<Asset> Asset;
    };

    /**
     * @brief Editor asset system for handling async asset loading
     * 
     * The EditorAssetSystem provides dedicated asset loading thread for editor builds.
     * It manages asset loading queues, file monitoring for hot-reload detection,
     * and async communication with the main thread.
     * 
     * This system enables non-blocking asset loading in the editor while maintaining
     * thread safety and providing efficient asset update mechanisms.
     */
    class EditorAssetSystem : public RefCounted
    {
    public:
        EditorAssetSystem();
        ~EditorAssetSystem();

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
         * @param metadata The asset metadata to load
         */
        void QueueAssetLoad(const AssetMetadata& metadata);

        /**
         * @brief Get an asset synchronously from the asset thread
         * @param metadata The asset metadata to load
         * @return Loaded asset or nullptr on failure
         */
        Ref<Asset> GetAsset(const AssetMetadata& metadata);

        /**
         * @brief Retrieve assets that have finished loading
         * @param outAssetList Output vector to fill with loaded assets
         * @return True if any assets were retrieved
         */
        bool RetrieveReadyAssets(std::vector<EditorAssetLoadResponse>& outAssetList);

        /**
         * @brief Update the loaded asset list (called from main thread)
         * @param loadedAssets Map of loaded assets to track
         */
        void UpdateLoadedAssetList(const std::unordered_map<AssetHandle, Ref<Asset>>& loadedAssets);

        /**
         * @brief Check if the asset thread is running
         * @return True if the thread is active
         */
        bool IsRunning() const { return m_Running; }

        /**
         * @brief Get asset update performance metrics
         * @return Time spent on asset updates in milliseconds
         */
        float GetAssetUpdatePerformance() const { return m_AssetUpdatePerf; }

    private:
        /**
         * @brief Asset monitor update (checks for file changes)
         */
        void AssetMonitorUpdate();

        /**
         * @brief Main asset thread function
         */
        void AssetThreadFunc();

        /**
         * @brief Ensure all loaded assets are current
         */
        void EnsureAllLoadedCurrent();

    private:
        Thread m_Thread;
        std::atomic<bool> m_Running = true;

        // Asset loading queue
        std::queue<AssetMetadata> m_AssetLoadingQueue;
        std::mutex m_AssetLoadingQueueMutex;
        std::condition_variable m_AssetLoadingQueueCV;

        // Ready assets queue (assets loaded and ready for main thread)
        std::queue<EditorAssetLoadResponse> m_ReadyAssets;
        std::mutex m_ReadyAssetsMutex;

        // Loaded assets tracking (for file change detection)
        std::unordered_map<AssetHandle, Ref<Asset>> m_LoadedAssets;
        std::mutex m_LoadedAssetsMutex;

        // Performance tracking
        float m_AssetUpdatePerf = 0.0f;
    };

} // namespace OloEngine
