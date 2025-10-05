#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <shared_mutex>
#include <atomic>
#include <filesystem>

namespace OloEngine
{
    /**
     * @brief Thread-safe asset metadata storage and management
     * 
     * The AssetRegistry maintains a central database of all asset metadata
     * in the project. It provides thread-safe access to asset information
     * and handles metadata persistence and loading.
     * 
     * Key features:
     * - Thread-safe metadata storage with UUID-based handle mapping
     * - Metadata persistence and loading from disk
     * - Registry iteration support for asset discovery
     * - Fast lookups by handle, path, and type
     * - Automatic handle generation for new assets
     */
    class AssetRegistry
    {
    public:
        AssetRegistry() = default;
        ~AssetRegistry() = default;

        // Disable copy operations - AssetRegistry manages stateful resources (mutex, atomic counter)
        AssetRegistry(const AssetRegistry&) = delete;
        AssetRegistry& operator=(const AssetRegistry&) = delete;

        // Disable move operations - std::shared_mutex is not movable
        AssetRegistry(AssetRegistry&&) = delete;
        AssetRegistry& operator=(AssetRegistry&&) = delete;

        /**
         * @brief Add or update asset metadata in the registry
         * @param metadata Asset metadata to store
         */
        void AddAsset(const AssetMetadata& metadata);

        /**
         * @brief Remove asset metadata from the registry
         * @param handle Handle of the asset to remove
         * @return True if asset was found and removed
         */
        bool RemoveAsset(AssetHandle handle);

        /**
         * @brief Get asset metadata by handle
         * @param handle Asset handle
         * @return Copy of asset metadata, or empty metadata if not found
         * 
         * Thread-safe: Returns by value to prevent dangling references after lock release.
         * The shared_lock is released before returning, so returning a reference would be unsafe
         * if another thread concurrently removes or modifies the metadata.
         */
        AssetMetadata GetMetadata(AssetHandle handle) const;

        /**
         * @brief Get asset metadata by file path
         * @param path File path of the asset
         * @return Copy of asset metadata, or empty metadata if not found
         * 
         * Thread-safe: Returns by value to prevent dangling references after lock release.
         * The shared_lock is released before returning, so returning a reference would be unsafe
         * if another thread concurrently removes or modifies the metadata.
         */
        AssetMetadata GetMetadata(const std::filesystem::path& path) const;

        /**
         * @brief Check if an asset exists in the registry
         * @param handle Asset handle
         * @return True if asset exists in registry
         */
        bool Exists(AssetHandle handle) const;

        /**
         * @brief Check if an asset path exists in the registry
         * @param path File path to check
         * @return True if asset with this path exists in registry
         */
        bool Exists(const std::filesystem::path& path) const;

        /**
         * @brief Get asset handle by file path
         * @param path File path of the asset
         * @return Asset handle, or 0 if not found
         */
        AssetHandle GetHandleFromPath(const std::filesystem::path& path) const;

        /**
         * @brief Get all assets of a specific type
         * @param type Asset type to search for
         * @return Vector of asset metadata for all assets of the specified type
         */
        std::vector<AssetMetadata> GetAssetsOfType(AssetType type) const;

        /**
         * @brief Get all asset handles of a specific type
         * @param type Asset type to search for
         * @return Set of asset handles of the specified type
         */
        std::unordered_set<AssetHandle> GetAssetHandlesOfType(AssetType type) const;

        /**
         * @brief Get all assets in the registry
         * @return Vector of all asset metadata
         */
        std::vector<AssetMetadata> GetAllAssets() const;

        /**
         * @brief Get the total number of assets in the registry
         * @return Number of assets
         */
        sizet GetAssetCount() const noexcept;

        /**
         * @brief Clear all assets from the registry
         */
        void Clear();

        /**
         * @brief Update asset metadata (preserving handle)
         * @param handle Asset handle
         * @param metadata New metadata (handle will be preserved)
         */
        void UpdateMetadata(AssetHandle handle, const AssetMetadata& metadata);

        /**
         * @brief Generate a new unique asset handle
         * @return New unique asset handle
         */
        AssetHandle GenerateHandle();

        /**
         * @brief Serialize the registry to a file
         * @param filepath Path where to save the registry
         * @return True if serialization was successful
         */
        bool Serialize(const std::filesystem::path& filepath) const;

        /**
         * @brief Deserialize the registry from a file
         * @param filepath Path to the registry file
         * @return True if deserialization was successful
         */
        bool Deserialize(const std::filesystem::path& filepath);

        /**
         * @brief Iterator support for range-based loops
         */
        auto begin() const { std::shared_lock lock(m_Mutex); return m_AssetMetadata.begin(); }
        auto end() const { std::shared_lock lock(m_Mutex); return m_AssetMetadata.end(); }

        /**
         * @brief Check if the registry is empty
         * @return True if no assets are registered
         */
        bool Empty() const;

    private:
        /**
         * @brief Get next available asset handle (thread-safe)
         * @return New unique handle
         * 
         * Uses atomic operations internally for thread safety. Can be called
         * concurrently from multiple threads without external synchronization.
         */
        AssetHandle GetNextHandle();

    private:
        // Main metadata storage (handle -> metadata)
        std::unordered_map<AssetHandle, AssetMetadata> m_AssetMetadata;
        
        // Fast path lookup (path -> handle)
        std::unordered_map<std::filesystem::path, AssetHandle> m_PathToHandle;
        
        // Handle generation counter (thread-safe atomic)
        std::atomic<u64> m_HandleCounter{1};
        
        // Thread synchronization
        mutable std::shared_mutex m_Mutex;
    };

}
