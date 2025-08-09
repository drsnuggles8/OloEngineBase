#include "EditorAssetManager.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"

#include <algorithm>
#include <future>
#include <filesystem>

namespace OloEngine
{
    EditorAssetManager::EditorAssetManager()
    {
#if OLO_ASYNC_ASSETS
        m_AssetThread = Ref<EditorAssetSystem>::Create();
#endif

        AssetImporter::Init();
        OLO_CORE_INFO("Initializing EditorAssetManager");
    }

    EditorAssetManager::~EditorAssetManager()
    {
        Shutdown();
    }

    void EditorAssetManager::Initialize()
    {
        OLO_CORE_INFO("EditorAssetManager initialized");
        
        // Load asset registry if it exists
        if (Project::GetActive())
        {
            auto registryPath = Project::GetAssetRegistryPath();
            std::error_code ec;
            if (std::filesystem::exists(registryPath, ec) && !ec)
            {
                m_AssetRegistry.Deserialize(registryPath);
                OLO_CORE_INFO("Loaded asset registry from {}", registryPath.string());
            }
            else if (ec)
            {
                OLO_CORE_WARN("Failed to check asset registry existence: {}", ec.message());
            }
        }

#if OLO_ASYNC_ASSETS
        // Start file watcher thread
        m_FileWatcherThread = std::thread([this]() { FileWatcherThreadFunction(); });
#endif
    }

    void EditorAssetManager::Shutdown()
    {
        OLO_CORE_INFO("Shutting down EditorAssetManager");

#if OLO_ASYNC_ASSETS
        // Stop asset thread
        if (m_AssetThread)
        {
            m_AssetThread->StopAndWait();
        }
        
        // Stop file watcher
        m_ShouldTerminate = true;
        if (m_FileWatcherThread.joinable())
        {
            m_FileWatcherThread.join();
        }
#endif

        // Save asset registry
        if (Project::GetActive())
        {
            auto registryPath = Project::GetAssetRegistryPath();
            m_AssetRegistry.Serialize(registryPath);
            OLO_CORE_INFO("Saved asset registry to {}", registryPath.string());
        }

        // Clear all loaded assets and memory assets
        {
            std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
            m_LoadedAssets.clear();
            m_MemoryAssets.clear();
        }
    }

    AssetType EditorAssetManager::GetAssetType(AssetHandle assetHandle)
    {
        if (!IsAssetHandleValid(assetHandle))
            return AssetType::None;

        auto metadata = m_AssetRegistry.GetMetadata(assetHandle);
        if (!metadata.IsValid())
            return AssetType::None;

        return metadata.Type;
    }

    Ref<Asset> EditorAssetManager::GetAsset(AssetHandle assetHandle)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::GetAsset");

        // Check both memory assets and loaded assets under a single lock
        {
            std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
            
            // Check if it's a memory asset first
            auto memoryIt = m_MemoryAssets.find(assetHandle);
            if (memoryIt != m_MemoryAssets.end())
                return memoryIt->second;

            // Check if already loaded
            auto loadedIt = m_LoadedAssets.find(assetHandle);
            if (loadedIt != m_LoadedAssets.end())
                return loadedIt->second;
        }

        // Load from file
        auto metadata = m_AssetRegistry.GetMetadata(assetHandle);
        if (!metadata.IsValid())
            return nullptr;
        
        return LoadAssetFromFile(metadata);
    }

    AsyncAssetResult<Asset> EditorAssetManager::GetAssetAsync(AssetHandle assetHandle)
    {
        // For editor, we typically load synchronously unless specifically requested
        // This can be enhanced later for true async loading
        auto asset = GetAsset(assetHandle);
        return AsyncAssetResult<Asset>{ asset, true }; // Always ready
    }

    void EditorAssetManager::AddMemoryOnlyAsset(Ref<Asset> asset)
    {
        if (!asset)
            return;

        std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
        m_MemoryAssets[asset->m_Handle] = asset;

        OLO_CORE_TRACE("Added memory asset: {}", (uint64_t)asset->m_Handle);
    }

    bool EditorAssetManager::ReloadData(AssetHandle assetHandle)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::ReloadData");

        auto metadata = m_AssetRegistry.GetMetadata(assetHandle);
        if (!metadata.IsValid())
        {
            OLO_CORE_ERROR("Cannot reload asset {}: metadata not found", (uint64_t)assetHandle);
            return false;
        }

        // Remove from cache to force reload
        {
            std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
            m_LoadedAssets.erase(assetHandle);
        }

        // Reload asset
        auto asset = LoadAssetFromFile(metadata);
        if (!asset)
        {
            OLO_CORE_ERROR("Failed to reload asset: {}", metadata.FilePath.string());
            return false;
        }

        // Update dependencies
        UpdateDependencies(assetHandle);

        OLO_CORE_INFO("Reloaded asset: {}", metadata.FilePath.string());
        return true;
    }

    void EditorAssetManager::ReloadDataAsync(AssetHandle assetHandle)
    {
        // For now, just do sync reload
        // TODO: Implement proper async reloading
        ReloadData(assetHandle);
    }

    bool EditorAssetManager::EnsureCurrent(AssetHandle assetHandle)
    {
        auto metadata = m_AssetRegistry.GetMetadata(assetHandle);
        if (!metadata.IsValid())
            return false;

        // Check if file exists before checking modification time
        if (!std::filesystem::exists(metadata.FilePath))
        {
            OLO_CORE_WARN("Asset file does not exist: {}", metadata.FilePath.string());
            return false;
        }

        // Check if file has been modified
        if (std::filesystem::last_write_time(metadata.FilePath) > metadata.LastWriteTime)
        {
            return ReloadData(assetHandle);
        }

        return true;
    }

    bool EditorAssetManager::EnsureAllLoadedCurrent()
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        bool allCurrent = true;

        for (const auto& [handle, asset] : m_LoadedAssets)
        {
            if (!EnsureCurrent(handle))
                allCurrent = false;
        }

        return allCurrent;
    }

    bool EditorAssetManager::IsAssetHandleValid(AssetHandle assetHandle)
    {
        return m_AssetRegistry.Exists(assetHandle);
    }

    Ref<Asset> EditorAssetManager::GetMemoryAsset(AssetHandle handle)
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        auto it = m_MemoryAssets.find(handle);
        return (it != m_MemoryAssets.end()) ? it->second : nullptr;
    }

    bool EditorAssetManager::IsAssetLoaded(AssetHandle handle)
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end();
    }

    bool EditorAssetManager::IsAssetValid(AssetHandle handle)
    {
        return IsAssetHandleValid(handle);
    }

    bool EditorAssetManager::IsAssetMissing(AssetHandle handle)
    {
        auto metadata = m_AssetRegistry.GetMetadata(handle);
        if (!metadata.IsValid())
            return true;

        return !std::filesystem::exists(metadata.FilePath);
    }

    bool EditorAssetManager::IsMemoryAsset(AssetHandle handle)
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        return m_MemoryAssets.find(handle) != m_MemoryAssets.end();
    }

    bool EditorAssetManager::IsPhysicalAsset(AssetHandle handle)
    {
        return IsAssetHandleValid(handle) && !IsMemoryAsset(handle);
    }

    void EditorAssetManager::RemoveAsset(AssetHandle handle)
    {
        // Remove from registry
        m_AssetRegistry.RemoveAsset(handle);

        // Remove from loaded assets and memory assets
        {
            std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
            m_LoadedAssets.erase(handle);
            m_MemoryAssets.erase(handle);
        }

        // Remove dependencies
        DeregisterDependencies(handle);

        OLO_CORE_TRACE("Removed asset: {}", (uint64_t)handle);
    }

    void EditorAssetManager::RegisterDependency(AssetHandle dependency, AssetHandle handle)
    {
        std::unique_lock<std::shared_mutex> lock(m_DependenciesMutex);
        m_AssetDependencies[dependency].insert(handle);
    }

    void EditorAssetManager::DeregisterDependency(AssetHandle dependency, AssetHandle handle)
    {
        std::unique_lock<std::shared_mutex> lock(m_DependenciesMutex);
        auto it = m_AssetDependencies.find(dependency);
        if (it != m_AssetDependencies.end())
        {
            it->second.erase(handle);
            if (it->second.empty())
                m_AssetDependencies.erase(it);
        }
    }

    void EditorAssetManager::DeregisterDependencies(AssetHandle handle)
    {
        std::unique_lock<std::shared_mutex> lock(m_DependenciesMutex);
        
        // Remove this asset from all dependency lists
        for (auto it = m_AssetDependencies.begin(); it != m_AssetDependencies.end();)
        {
            it->second.erase(handle);
            if (it->second.empty())
                it = m_AssetDependencies.erase(it);
            else
                ++it;
        }

        // Remove dependencies of this asset
        m_AssetDependencies.erase(handle);
    }

    std::unordered_set<AssetHandle> EditorAssetManager::GetDependencies(AssetHandle handle)
    {
        std::shared_lock<std::shared_mutex> lock(m_DependenciesMutex);
        auto it = m_AssetDependencies.find(handle);
        return (it != m_AssetDependencies.end()) ? it->second : std::unordered_set<AssetHandle>{};
    }

    AssetHandle EditorAssetManager::ImportAsset(const std::filesystem::path& filepath)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::ImportAsset");

        if (!std::filesystem::exists(filepath))
        {
            OLO_CORE_ERROR("Cannot import asset: file does not exist: {}", filepath.string());
            return 0;
        }

        AssetType type = AssetExtensions::GetAssetTypeFromPath(filepath.string());
        if (type == AssetType::None)
        {
            OLO_CORE_ERROR("Cannot import asset: unsupported file type: {}", filepath.string());
            return 0;
        }

        // Check if already imported
        AssetHandle existingHandle = m_AssetRegistry.GetHandleFromPath(filepath);
        if (existingHandle != 0)
        {
            OLO_CORE_WARN("Asset already imported: {}", filepath.string());
            return existingHandle;
        }

        // Create metadata
        AssetMetadata metadata;
        metadata.Handle = AssetHandle{};
        metadata.FilePath = filepath;
        metadata.Type = type;
        metadata.LastWriteTime = std::filesystem::last_write_time(filepath);

        // Register in registry
        m_AssetRegistry.AddAsset(metadata);

        OLO_CORE_INFO("Imported asset: {} -> {}", filepath.string(), (uint64_t)metadata.Handle);
        return metadata.Handle;
    }

    void EditorAssetManager::SyncWithAssetThread()
    {
        // In editor mode, we don't have a separate asset thread
        // This is a no-op for compatibility
    }

    Ref<Asset> EditorAssetManager::LoadAssetFromFile(const AssetMetadata& metadata)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::LoadAsset");

        if (!metadata.IsValid())
        {
            OLO_CORE_ERROR("Cannot load asset: invalid metadata");
            return nullptr;
        }

        if (!std::filesystem::exists(metadata.FilePath))
        {
            OLO_CORE_ERROR("Cannot load asset: file does not exist: {}", metadata.FilePath.string());
            return nullptr;
        }

        // Load asset using importer
        Ref<Asset> asset;
        if (!AssetImporter::TryLoadData(metadata, asset))
        {
            OLO_CORE_ERROR("Failed to load asset: {}", metadata.FilePath.string());
            return nullptr;
        }

        // Cache the loaded asset
        {
            std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
            m_LoadedAssets[metadata.Handle] = asset;
        }

        OLO_CORE_TRACE("Loaded asset: {}", metadata.FilePath.string());
        return asset;
    }

    void EditorAssetManager::UpdateDependencies(AssetHandle handle)
    {
        // First, gather the dependent handles while holding the dependency lock
        std::unordered_set<AssetHandle> dependents;
        {
            std::shared_lock<std::shared_mutex> lock(m_DependenciesMutex);
            auto it = m_AssetDependencies.find(handle);
            if (it != m_AssetDependencies.end())
            {
                dependents = it->second;
            }
        }
        
        // Then iterate over dependents without holding any locks to prevent deadlock
        for (AssetHandle dependent : dependents)
        {
            // Notify dependent assets that this asset has changed
            auto asset = GetAsset(dependent);
            if (asset)
            {
                asset->OnDependencyUpdated(handle);
            }
        }
    }

#if OLO_ASYNC_ASSETS
    void EditorAssetManager::FileWatcherThreadFunction()
    {
        OLO_CORE_INFO("File watcher thread started");

        while (!m_ShouldTerminate)
        {
            // TODO: Implement file watching logic using FileWatch library
            // For now, just sleep and check periodically
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        OLO_CORE_INFO("File watcher thread stopped");
    }
#endif

    std::unordered_map<AssetHandle, Ref<Asset>> EditorAssetManager::GetLoadedAssetsCopy() const
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        return m_LoadedAssets;
    }

} // namespace OloEngine
