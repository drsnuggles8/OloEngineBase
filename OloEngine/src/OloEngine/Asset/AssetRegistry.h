#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Threading/SharedMutex.h"
#include "OloEngine/Threading/SharedLock.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
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
        // On-disk .oar format version. Deserialize accepts exactly this version; any other file
        // is rejected and has to be rebuilt from the project (docs/agent-rules/binary-format-versioning.md).
        // v3 (#1644): an entry no longer carries the runtime AssetStatus.
        static constexpr u32 FileVersion = 3;

        AssetRegistry() = default;
        ~AssetRegistry() = default;

        // Disable copy operations - AssetRegistry manages stateful resources (mutex, atomic counter)
        AssetRegistry(const AssetRegistry&) = delete;
        AssetRegistry& operator=(const AssetRegistry&) = delete;

        // Disable move operations - FSharedMutex is not movable
        AssetRegistry(AssetRegistry&&) = delete;
        AssetRegistry& operator=(AssetRegistry&&) = delete;

        // ── Cache artefacts (#1664) ──
        // A file under a directory named `cache` (the `.gitignore` rule `**/cache/**`) is
        // derived data: a cooked embedded texture under Assets/cache/embedded/, for one. Its
        // file is never committed, so its registry entry must never be either, or a fresh
        // clone holds an entry for a file it does not have. Such an entry therefore:
        //   * has the handle CacheArtefactHandle(path), derived from its registry path, so every
        //     manager that registers the file (an editor session, the pack cook, a test)
        //     gives it the same handle without reading it from anywhere;
        //   * is held in memory like any other entry, but Serialize() never writes it, and
        //     Deserialize() drops one found in a file;
        //   * is refused by AddAsset/UpdateMetadata under any other handle, because that
        //     handle could not be re-derived after a restart.
        // The directory name is matched ignoring case, on every platform, so a registry means the
        // same everywhere; .gitignore ignores `[Cc][Aa][Cc][Hh][Ee]/` to match.
        [[nodiscard]] static bool IsCacheArtefact(const std::filesystem::path& path);
        // Nonzero for every path. FNV-1a-64 of the path's generic spelling.
        [[nodiscard]] static AssetHandle CacheArtefactHandle(const std::filesystem::path& path);
        // The handle to give a NEW entry for this registry path: CacheArtefactHandle for a
        // cache artefact, a fresh random one otherwise. Every caller that mints a handle for a
        // path it is about to register takes it from here. A random handle, unlike the counter,
        // cannot reissue a handle dropped with an older registry file's cache entry.
        [[nodiscard]] static AssetHandle HandleForNewEntry(const std::filesystem::path& path);
        // Model's cook writes `<stem>.tmp<hex>.png` beside the cooked file and renames it into
        // place when the write is complete. Such a file is never registered.
        [[nodiscard]] static bool IsInFlightCookFile(const std::filesystem::path& path);

        /**
         * @brief Add or update asset metadata in the registry
         * @param metadata Asset metadata to store
         * @return False when the entry was refused (a zero handle, a cache artefact whose
         *         handle is not CacheArtefactHandle of its path, or a cache artefact whose handle
         *         already belongs to another path, a file moved out of the cache); the refusal is
         *         logged as an error.
         */
        bool AddAsset(const AssetMetadata& metadata);

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
         * @return False when the handle is unknown or the new path is a cache artefact whose
         *         derived handle is not this one (both logged); nothing is changed then.
         */
        bool UpdateMetadata(AssetHandle handle, const AssetMetadata& metadata);

        /**
         * @brief Generate a new unique asset handle
         * @return New unique asset handle
         */
        AssetHandle GenerateHandle();

        /**
         * @brief Serialize the registry to a file
         *
         * Writes every entry except cache artefacts (IsCacheArtefact), which are re-derived
         * by whoever registers their files and never belong in the tracked registry.
         *
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
         * @brief Iterate over all assets with a callback while holding the lock
         *
         * This is the thread-safe way to iterate over assets. The callback is invoked
         * for each asset while the shared lock is held, preventing data races.
         *
         * @param callback Function to call for each asset. Return false to stop iteration early.
         */
        template<typename Func>
        void ForEachAsset(Func&& callback) const
        {
            TSharedLock<FSharedMutex> lock(m_Mutex);
            for (const auto& [handle, metadata] : m_AssetMetadata)
            {
                if (!callback(handle, metadata))
                {
                    break;
                }
            }
        }

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

        // Shared by AddAsset and UpdateMetadata; logs the refusal.
        static bool IsAcceptableHandleForPath(AssetHandle handle, const std::filesystem::path& path, const char* caller);

      private:
        // Main metadata storage (handle -> metadata)
        std::unordered_map<AssetHandle, AssetMetadata> m_AssetMetadata;

        // Fast path lookup (path -> handle)
        std::unordered_map<std::filesystem::path, AssetHandle> m_PathToHandle;

        // Handle generation counter (thread-safe atomic)
        std::atomic<u64> m_HandleCounter{ 1 };

        // Thread synchronization
        mutable FSharedMutex m_Mutex;
    };

} // namespace OloEngine
