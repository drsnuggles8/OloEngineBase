#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Threading/Mutex.h"

#include <queue>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <tuple>

namespace OloEngine
{
    /**
     * @brief Raw asset data pending GPU finalization
     *
     * This holds intermediate asset data loaded from disk on a worker thread,
     * waiting to be finalized on the main thread (where GPU resources are created).
     */
    struct PendingRawAsset
    {
        AssetMetadata Metadata;
        RawAssetData RawData;
        AssetType SerializerType = AssetType::None; ///< Type of serializer to use for finalization
    };

    /**
     * @brief Editor asset system for handling async asset loading
     *
     * The EditorAssetSystem provides dedicated asset loading for editor builds.
     * It manages asset loading tasks, file monitoring for hot-reload detection,
     * and async communication with the main thread.
     *
     * For assets that support async loading (textures, shaders), the system uses
     * a two-phase approach:
     * 1. Worker threads load raw data from disk (no GPU calls)
     * 2. Main thread finalizes GPU resources when retrieving ready assets
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
         * @brief Stop the asset system (cancels pending tasks if possible)
         */
        void Stop();

        /**
         * @brief Stop the asset system and wait for completion
         */
        void StopAndWait();

        /**
         * @brief Queue an asset for loading
         * @param metadata The asset metadata to load
         */
        void QueueAssetLoad(const AssetMetadata& metadata);

        /**
         * @brief Get an asset synchronously (blocking)
         * @param metadata The asset metadata to load
         * @return Loaded asset or nullptr on failure
         *
         * Note: For assets that support async loading, this still works but
         * creates GPU resources on the calling thread. Use QueueAssetLoad
         * for proper async loading.
         */
        Ref<Asset> GetAsset(const AssetMetadata& metadata);

        /**
         * @brief Retrieve assets that have finished loading
         * @param outAssetList Output vector to fill with loaded assets
         * @return True if any assets were retrieved
         *
         * For assets with NeedsGPUFinalization=true, the caller (EditorAssetManager)
         * must call FinalizeFromRawData on the main thread.
         */
        bool RetrieveReadyAssets(std::vector<EditorAssetLoadResponse>& outAssetList);

        /**
         * @brief Retrieve raw assets pending GPU finalization
         * @param outRawAssets Output vector to fill with pending raw assets
         * @return True if any raw assets were retrieved
         *
         * Call this from the main thread, then finalize GPU resources.
         */
        bool RetrievePendingRawAssets(std::vector<PendingRawAsset>& outRawAssets);

        /**
         * @brief Update the loaded asset list (called from main thread)
         * @param loadedAssets Map of loaded assets to track
         */
        void UpdateLoadedAssetList(const std::unordered_map<AssetHandle, Ref<Asset>>& loadedAssets);

        /**
         * @brief Check if the asset system is running
         * @return True if active
         */
        bool IsRunning() const
        {
            return m_Running;
        }

        /**
         * @brief Get asset update performance metrics
         * @return Time spent on asset updates in milliseconds
         */
        float GetAssetUpdatePerformance() const
        {
            return m_AssetUpdatePerf;
        }

        /**
         * @brief Get telemetry information for debugging
         * @return Tuple of (queued count, loaded count, failed count, queue length)
         */
        std::tuple<u32, u32, u32, sizet> GetTelemetry() const;

        /**
         * @brief Get current queue length
         * @return Number of assets currently in loading queue
         */
        sizet GetQueueLength() const;

      private:
        /**
         * @brief Ensure all loaded assets are current
         */
        void EnsureAllLoadedCurrent();

      private:
        std::atomic<bool> m_Running = true;

        // Ready assets queue (fully loaded assets ready for main thread)
        std::queue<EditorAssetLoadResponse> m_ReadyAssets;
        mutable FMutex m_ReadyAssetsMutex;

        // Pending raw assets (need GPU finalization on main thread)
        std::queue<PendingRawAsset> m_PendingRawAssets;
        mutable FMutex m_PendingRawAssetsMutex;

        // Loaded assets tracking (for file change detection)
        std::unordered_map<AssetHandle, Ref<Asset>> m_LoadedAssets;
        FMutex m_LoadedAssetsMutex;

        // Pending assets tracking (to prevent duplicate loading)
        std::unordered_set<AssetHandle> m_PendingAssets;
        FMutex m_PendingAssetsMutex;

        // Performance tracking
        float m_AssetUpdatePerf = 0.0f;

        // Telemetry counters
        mutable std::atomic<u32> m_QueuedAssetsCount{ 0 };
        mutable std::atomic<u32> m_LoadedAssetsCount{ 0 };
        mutable std::atomic<u32> m_FailedAssetsCount{ 0 };
        mutable std::atomic<sizet> m_ActiveTaskCount{ 0 };
    };

} // namespace OloEngine
