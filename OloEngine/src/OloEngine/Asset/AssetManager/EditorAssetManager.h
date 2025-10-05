#pragma once

#include "AssetManagerBase.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetSystem/EditorAssetSystem.h"
#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Core/Application.h"

#include <shared_mutex>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <type_traits>
#include <utility>

#if OLO_ASYNC_ASSETS
#include <thread>
#include <atomic>
#include "FileWatch.hpp"
#endif

namespace OloEngine
{
    /**
     * @brief Editor asset manager with file-based loading and hot-reload support
     * 
     * The EditorAssetManager is designed for development builds where assets
     * are loaded from individual files and can be hot-reloaded when modified.
     * It provides:
     * 
     * - File-based asset loading with hot-reload
     * - Asset registry management and serialization
     * - File system monitoring and change detection
     * - Asynchronous asset loading thread
     * - Asset metadata caching and validation
     * - Comprehensive dependency tracking and resolution
     * 
     * This manager is more complex than RuntimeAssetManager but provides
     * the flexibility needed during development.
     */
    class EditorAssetManager : public AssetManagerBase
    {
    public:
        EditorAssetManager();
        virtual ~EditorAssetManager();

        // AssetManagerBase interface implementation
        virtual void Initialize();
        virtual void Shutdown() noexcept override;

        virtual AssetType GetAssetType(AssetHandle assetHandle) const noexcept override;
        virtual Ref<Asset> GetAsset(AssetHandle assetHandle) override;
        virtual AsyncAssetResult<Asset> GetAssetAsync(AssetHandle assetHandle) override;

        virtual void AddMemoryOnlyAsset(Ref<Asset> asset) override;
        virtual [[nodiscard]] bool ReloadData(AssetHandle assetHandle) override;
        virtual void ReloadDataAsync(AssetHandle assetHandle) override;
        virtual [[nodiscard]] bool EnsureCurrent(AssetHandle assetHandle) override;
        virtual [[nodiscard]] bool EnsureAllLoadedCurrent() override;
        virtual [[nodiscard]] bool IsAssetHandleValid(AssetHandle assetHandle) const noexcept override;
        virtual Ref<Asset> GetMemoryAsset(AssetHandle handle) const override;
        virtual [[nodiscard]] bool IsAssetLoaded(AssetHandle handle) const noexcept override;
        virtual [[nodiscard]] bool IsAssetValid(AssetHandle handle) const noexcept override;
        virtual [[nodiscard]] bool IsAssetMissing(AssetHandle handle) const noexcept override;
        virtual [[nodiscard]] bool IsMemoryAsset(AssetHandle handle) const noexcept override;
        virtual [[nodiscard]] bool IsPhysicalAsset(AssetHandle handle) const noexcept override;
        virtual void RemoveAsset(AssetHandle handle) override;

        // Dependency management
        virtual void RegisterDependency(AssetHandle handle, AssetHandle dependency) override;
        virtual void DeregisterDependency(AssetHandle handle, AssetHandle dependency) override;
        virtual void DeregisterDependencies(AssetHandle handle) override;
        virtual std::unordered_set<AssetHandle> GetDependencies(AssetHandle handle) const override;

        virtual void SyncWithAssetThread() noexcept override;

        virtual std::unordered_set<AssetHandle> GetAllAssetsWithType(AssetType type) const override;
        
        virtual std::unordered_map<AssetHandle, Ref<Asset>> GetLoadedAssets() const override;
        virtual void ForEachLoadedAsset(const std::function<bool(AssetHandle, const Ref<Asset>&)>& callback) const override;

        /**
         * @brief Get a copy of loaded assets for safe iteration in multithreaded contexts
         * @return Copy of the loaded assets map
         * @deprecated Use GetLoadedAssets() instead, which now returns a safe copy
         */
        std::unordered_map<AssetHandle, Ref<Asset>> GetLoadedAssetsCopy() const;

        // Editor-specific methods

        /**
         * @brief Get asset metadata by handle
         * @param handle Asset handle
         * @return Copy of asset metadata (thread-safe - returns by value to prevent dangling reference)
         */
        AssetMetadata GetMetadata(AssetHandle handle) const;

        /**
         * @brief Get asset metadata by handle (base class override)
         * @param handle Asset handle
         * @return Copy of asset metadata (thread-safe)
         */
        virtual AssetMetadata GetAssetMetadata(AssetHandle handle) const noexcept override { return GetMetadata(handle); }

        /**
         * @brief Set asset metadata (thread-safe)
         * @param handle Asset handle
         * @param metadata New metadata to set
         */
        void SetMetadata(AssetHandle handle, const AssetMetadata& metadata);

        /**
         * @brief Update asset status (thread-safe)
         * @param handle Asset handle
         * @param status New status
         */
        void SetAssetStatus(AssetHandle handle, AssetStatus status);

        /**
         * @brief Get asset handle from file path
         * @param filepath File path to look up
         * @return Asset handle or 0 if not found
         */
        AssetHandle GetAssetHandleFromFilePath(const std::filesystem::path& filepath);

        /**
         * @brief Get asset type from file extension
         * @param extension File extension (with or without leading dot)
         * @return Asset type or AssetType::None if not recognized
         */
        AssetType GetAssetTypeFromExtension(const std::string& extension);

        /**
         * @brief Get default file extension for asset type
         * @param type Asset type
         * @return Default extension for the type
         */
        std::string GetDefaultExtensionForAssetType(AssetType type);

        /**
         * @brief Get asset type from file path
         * @param path File path to analyze
         * @return Asset type based on file extension
         */
        AssetType GetAssetTypeFromPath(const std::filesystem::path& path);

        /**
         * @brief Get file system path from asset handle
         * @param handle Asset handle
         * @return Absolute file system path
         */
        std::filesystem::path GetFileSystemPath(AssetHandle handle);

        /**
         * @brief Get file system path from asset metadata
         * @param metadata Asset metadata
         * @return Absolute file system path
         */
        std::filesystem::path GetFileSystemPath(const AssetMetadata& metadata);

        /**
         * @brief Get file system path as string
         * @param metadata Asset metadata
         * @return Absolute file system path as string
         */
        std::string GetFileSystemPathString(const AssetMetadata& metadata);

        /**
         * @brief Get project-relative path from absolute path
         * @param filepath Absolute file path
         * @return Project-relative path
         */
        std::filesystem::path GetRelativePath(const std::filesystem::path& filepath);

        /**
         * @brief Check if file exists for given metadata
         * @param metadata Asset metadata to check
         * @return True if file exists on disk
         */
        bool FileExists(AssetMetadata& metadata) const;

        /**
         * @brief Create or replace an asset of specified type
         * @tparam T Asset type to create
         * @tparam Args Constructor arguments
         * @param path File path for the asset
         * @param args Constructor arguments for the asset
         * @return Created/replaced asset
         */
        template<typename T, typename... Args>
        Ref<T> CreateOrReplaceAsset(const std::filesystem::path& path, Args&&... args)
        {
            static_assert(std::is_base_of<Asset, T>::value, "CreateOrReplaceAsset only works for types derived from Asset");

            // Check if asset for this file already exists.
            // If it does, and its the same type we just replace existing asset
            // Otherwise we create a whole new asset.
            auto relativePath = GetRelativePath(path);
            auto handle = GetAssetHandleFromFilePath(relativePath);
            AssetMetadata metadata = handle ? GetMetadata(handle) : AssetMetadata{};
            if (metadata.Type != T::GetStaticType())
            {
                metadata = {};
            }

            bool replaceAsset = false;
            if (metadata.Handle == 0)
            {
                metadata.Handle = AssetHandle(); // Generate a fresh unique handle
                metadata.FilePath = relativePath;
                metadata.Type = T::GetStaticType();
                metadata.IsDataLoaded = true;
                SetMetadata(metadata.Handle, metadata);
                SerializeAssetRegistry();
            }
            else
            {
                replaceAsset = true;
            }

            Ref<T> asset = Ref<T>::Create(std::forward<Args>(args)...);
            asset->Handle = metadata.Handle;
            m_LoadedAssets[asset->Handle] = asset;
            AssetImporter::Serialize(metadata, asset);

            // Read serialized timestamp
            auto absolutePath = GetFileSystemPath(metadata);
            metadata.LastWriteTime = std::filesystem::last_write_time(absolutePath);
            SetMetadata(metadata.Handle, metadata);

            if (replaceAsset)
            {
                OLO_CORE_INFO_TAG("AssetManager", "Replaced asset {}", metadata.FilePath.string());
                UpdateDependencies(metadata.Handle);
                // Dispatch AssetReloadedEvent on main thread so UI layers can handle it safely
                auto handle = metadata.Handle;
                auto type = metadata.Type;
                auto path = metadata.FilePath;
                Application::Get().SubmitToMainThread([handle, type, path]() mutable {
                    AssetReloadedEvent evt(handle, type, path);
                    Application::Get().OnEvent(evt);
                });
            }

            return asset;
        }

        /**
         * @brief Replace a loaded asset (for hot-reload)
         * @param handle Asset handle
         * @param newAsset New asset to replace with
         */
        void ReplaceLoadedAsset(AssetHandle handle, Ref<Asset> newAsset);

        /**
         * @brief Write asset registry to file
         * @return True if successful
         */
        bool WriteRegistryToFile();

        /**
         * @brief Import an asset from a file path
         * @param filepath Path to the asset file
         * @return Handle of the imported asset, or 0 if failed
         */
        AssetHandle ImportAsset(const std::filesystem::path& filepath);

        /**
         * @brief Create a new asset of the specified type
         * @param type Asset type to create
         * @param path Path where the asset should be saved
         * @return Handle of the created asset, or 0 if failed
         */
        AssetHandle CreateAsset(AssetType type, const std::filesystem::path& path);

        /**
         * @brief Serialize the asset registry to disk
         * @return True if serialization was successful
         */
        bool SerializeAssetRegistry();

        /**
         * @brief Deserialize the asset registry from disk
         * @return True if deserialization was successful
         */
        bool DeserializeAssetRegistry();

        /**
         * @brief Process file system events (for hot-reload)
         * @param event File system event to process
         */
        void ProcessFileSystemEvent(const FileSystemEvent& event);

        /**
         * @brief Regenerate asset registry by scanning the project directory
         */
        void RegenerateAssetRegistry();

        /**
         * @brief Update dependents when an asset changes (for hot-reload)
         * @param handle Asset handle that changed
         */
        void UpdateDependencies(AssetHandle handle);

        /**
         * @brief Notify dependent assets when this asset has been updated
         * @param handle Asset handle that was updated
         */
        void UpdateDependents(AssetHandle handle);

    private:
        /**
         * @brief Load an asset from file
         * @param metadata Asset metadata containing file path
         * @return Loaded asset or nullptr if failed
         */
        Ref<Asset> LoadAssetFromFile(const AssetMetadata& metadata);

        /**
         * @brief Update asset dependencies when loading
         * @param handle Asset handle
         * @param asset Loaded asset
         */
        void UpdateAssetDependencies(AssetHandle handle, Ref<Asset> asset);

        /**
         * @brief Check if a file has been modified since last load
         * @param metadata Asset metadata to check
         * @return True if file has been modified
         */
        bool IsFileModified(const AssetMetadata& metadata) const;

        /**
         * @brief Scan directory for assets and update registry
         * @param directory Directory to scan
         */
        void ScanDirectoryForAssets(const std::filesystem::path& directory);

    private:
        // Asset registry for metadata management
        AssetRegistry m_AssetRegistry;
        
        // Loaded assets cache
        std::unordered_map<AssetHandle, Ref<Asset>> m_LoadedAssets;
        
        // Memory-only assets (no backing file)
        std::unordered_map<AssetHandle, Ref<Asset>> m_MemoryAssets;
        
        // Asset dependencies tracking
        std::unordered_map<AssetHandle, std::unordered_set<AssetHandle>> m_AssetDependencies; // asset handle -> assets that it depends on
        std::unordered_map<AssetHandle, std::unordered_set<AssetHandle>> m_AssetDependents;   // asset handle -> assets that depend on it
        
        // Async asset loading system
        Ref<EditorAssetSystem> m_AssetThread;
        
        // Thread synchronization
        mutable std::shared_mutex m_AssetsMutex;
        mutable std::shared_mutex m_RegistryMutex;
        mutable std::shared_mutex m_DependenciesMutex;
        
#if OLO_ASYNC_ASSETS
        // File watching thread and control
        std::thread m_FileWatcherThread;
        std::atomic<bool> m_ShouldTerminate{false};
        
        // Real-time file watching using filewatch library
        std::unique_ptr<filewatch::FileWatch<std::string>> m_ProjectFileWatcher;
        
        // Callback for file system events
        void OnFileSystemEvent(const std::string& file, const filewatch::Event change_type);
        
        // File watcher thread function
        void FileWatcherThreadFunction();
#endif
        
        // Project path for asset scanning
        std::filesystem::path m_ProjectPath;
    };

}
