#include "EditorAssetManager.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/FileSystem.h"

#include <algorithm>
#include <future>

namespace OloEngine
{
    EditorAssetManager::EditorAssetManager()
        : m_AssetRegistry(Ref<AssetRegistry>::Create())
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
        const auto& project = Application::Get().GetProject();
        if (project)
        {
            auto registryPath = project->GetAssetRegistryPath();
            if (std::filesystem::exists(registryPath))
            {
                m_AssetRegistry->Deserialize(registryPath);
                OLO_CORE_INFO("Loaded asset registry from {}", registryPath.string());
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
        const auto& project = Application::Get().GetProject();
        if (project && m_AssetRegistry)
        {
            auto registryPath = project->GetAssetRegistryPath();
            m_AssetRegistry->Serialize(registryPath);
            OLO_CORE_INFO("Saved asset registry to {}", registryPath.string());
        }

        // Clear all loaded assets
        {
            std::unique_lock lock(m_LoadedAssetsMutex);
            m_LoadedAssets.clear();
        }

        // Clear memory assets
        {
            std::unique_lock lock(m_MemoryAssetsMutex);
            m_MemoryAssets.clear();
        }
    }

    AssetType EditorAssetManager::GetAssetType(AssetHandle assetHandle)
    {
        if (!IsAssetHandleValid(assetHandle))
            return AssetType::None;

        return m_AssetRegistry->GetAssetType(assetHandle);
    }

    Ref<Asset> EditorAssetManager::GetAsset(AssetHandle assetHandle)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::GetAsset");

        // Check if it's a memory asset first
        {
            std::shared_lock lock(m_MemoryAssetsMutex);
            auto it = m_MemoryAssets.find(assetHandle);
            if (it != m_MemoryAssets.end())
                return it->second;
        }

        // Check if already loaded
        {
            std::shared_lock lock(m_LoadedAssetsMutex);
            auto it = m_LoadedAssets.find(assetHandle);
            if (it != m_LoadedAssets.end())
                return it->second;
        }

        // Load from file
        return LoadAsset(assetHandle);
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

        std::unique_lock lock(m_MemoryAssetsMutex);
        m_MemoryAssets[asset->GetHandle()] = asset;
        
        OLO_CORE_TRACE("Added memory asset: {}", (uint64_t)asset->GetHandle());
    }

    bool EditorAssetManager::ReloadData(AssetHandle assetHandle)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::ReloadData");

        auto metadata = m_AssetRegistry->GetAssetMetadata(assetHandle);
        if (!metadata.IsValid())
        {
            OLO_CORE_ERROR("Cannot reload asset {}: metadata not found", (uint64_t)assetHandle);
            return false;
        }

        // Remove from cache to force reload
        {
            std::unique_lock lock(m_LoadedAssetsMutex);
            m_LoadedAssets.erase(assetHandle);
        }

        // Reload asset
        auto asset = LoadAsset(assetHandle);
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
        auto metadata = m_AssetRegistry->GetAssetMetadata(assetHandle);
        if (!metadata.IsValid())
            return false;

        // Check if file has been modified
        if (FileSystem::GetLastWriteTime(metadata.FilePath) > metadata.LastModified)
        {
            return ReloadData(assetHandle);
        }

        return true;
    }

    bool EditorAssetManager::EnsureAllLoadedCurrent()
    {
        std::shared_lock lock(m_LoadedAssetsMutex);
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
        return m_AssetRegistry->Contains(assetHandle);
    }

    Ref<Asset> EditorAssetManager::GetMemoryAsset(AssetHandle handle)
    {
        std::shared_lock lock(m_MemoryAssetsMutex);
        auto it = m_MemoryAssets.find(handle);
        return (it != m_MemoryAssets.end()) ? it->second : nullptr;
    }

    bool EditorAssetManager::IsAssetLoaded(AssetHandle handle)
    {
        std::shared_lock lock(m_LoadedAssetsMutex);
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end();
    }

    bool EditorAssetManager::IsAssetValid(AssetHandle handle)
    {
        return IsAssetHandleValid(handle);
    }

    bool EditorAssetManager::IsAssetMissing(AssetHandle handle)
    {
        auto metadata = m_AssetRegistry->GetAssetMetadata(handle);
        if (!metadata.IsValid())
            return true;

        return !std::filesystem::exists(metadata.FilePath);
    }

    bool EditorAssetManager::IsMemoryAsset(AssetHandle handle)
    {
        std::shared_lock lock(m_MemoryAssetsMutex);
        return m_MemoryAssets.find(handle) != m_MemoryAssets.end();
    }

    bool EditorAssetManager::IsPhysicalAsset(AssetHandle handle)
    {
        return IsAssetHandleValid(handle) && !IsMemoryAsset(handle);
    }

    void EditorAssetManager::RemoveAsset(AssetHandle handle)
    {
        // Remove from registry
        m_AssetRegistry->Remove(handle);

        // Remove from loaded assets
        {
            std::unique_lock lock(m_LoadedAssetsMutex);
            m_LoadedAssets.erase(handle);
        }

        // Remove from memory assets
        {
            std::unique_lock lock(m_MemoryAssetsMutex);
            m_MemoryAssets.erase(handle);
        }

        // Remove dependencies
        DeregisterDependencies(handle);

        OLO_CORE_TRACE("Removed asset: {}", (uint64_t)handle);
    }

    void EditorAssetManager::RegisterDependency(AssetHandle dependency, AssetHandle handle)
    {
        std::unique_lock lock(m_DependenciesMutex);
        m_Dependencies[dependency].insert(handle);
    }

    void EditorAssetManager::DeregisterDependency(AssetHandle dependency, AssetHandle handle)
    {
        std::unique_lock lock(m_DependenciesMutex);
        auto it = m_Dependencies.find(dependency);
        if (it != m_Dependencies.end())
        {
            it->second.erase(handle);
            if (it->second.empty())
                m_Dependencies.erase(it);
        }
    }

    void EditorAssetManager::DeregisterDependencies(AssetHandle handle)
    {
        std::unique_lock lock(m_DependenciesMutex);
        
        // Remove this asset from all dependency lists
        for (auto it = m_Dependencies.begin(); it != m_Dependencies.end();)
        {
            it->second.erase(handle);
            if (it->second.empty())
                it = m_Dependencies.erase(it);
            else
                ++it;
        }

        // Remove dependencies of this asset
        m_Dependencies.erase(handle);
    }

    std::unordered_set<AssetHandle> EditorAssetManager::GetDependencies(AssetHandle handle)
    {
        std::shared_lock lock(m_DependenciesMutex);
        auto it = m_Dependencies.find(handle);
        return (it != m_Dependencies.end()) ? it->second : std::unordered_set<AssetHandle>{};
    }

    std::unordered_map<AssetHandle, Ref<Asset>> EditorAssetManager::GetLoadedAssets()
    {
        std::shared_lock lock(m_LoadedAssetsMutex);
        return m_LoadedAssets;
    }

    AssetHandle EditorAssetManager::ImportAsset(const std::filesystem::path& filepath)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::ImportAsset");

        if (!std::filesystem::exists(filepath))
        {
            OLO_CORE_ERROR("Cannot import asset: file does not exist: {}", filepath.string());
            return 0;
        }

        AssetType type = AssetExtensions::GetAssetTypeFromPath(filepath);
        if (type == AssetType::None)
        {
            OLO_CORE_ERROR("Cannot import asset: unsupported file type: {}", filepath.string());
            return 0;
        }

        // Check if already imported
        AssetHandle existingHandle = m_AssetRegistry->GetAssetHandle(filepath);
        if (existingHandle != 0)
        {
            OLO_CORE_WARN("Asset already imported: {}", filepath.string());
            return existingHandle;
        }

        // Create metadata
        AssetMetadata metadata;
        metadata.Handle = AssetHandle::Create();
        metadata.FilePath = filepath;
        metadata.Type = type;
        metadata.LastModified = FileSystem::GetLastWriteTime(filepath);

        // Register in registry
        m_AssetRegistry->RegisterAsset(metadata);

        OLO_CORE_INFO("Imported asset: {} -> {}", filepath.string(), (uint64_t)metadata.Handle);
        return metadata.Handle;
    }

    void EditorAssetManager::SyncWithAssetThread()
    {
        // In editor mode, we don't have a separate asset thread
        // This is a no-op for compatibility
    }

    Ref<Asset> EditorAssetManager::LoadAsset(AssetHandle handle)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::LoadAsset");

        auto metadata = m_AssetRegistry->GetAssetMetadata(handle);
        if (!metadata.IsValid())
        {
            OLO_CORE_ERROR("Cannot load asset: metadata not found for handle {}", (uint64_t)handle);
            return nullptr;
        }

        if (!std::filesystem::exists(metadata.FilePath))
        {
            OLO_CORE_ERROR("Cannot load asset: file does not exist: {}", metadata.FilePath.string());
            return nullptr;
        }

        // Load asset using importer
        auto asset = AssetImporter::ImportAsset(handle, metadata);
        if (!asset)
        {
            OLO_CORE_ERROR("Failed to load asset: {}", metadata.FilePath.string());
            return nullptr;
        }

        // Cache the loaded asset
        {
            std::unique_lock lock(m_LoadedAssetsMutex);
            m_LoadedAssets[handle] = asset;
        }

        OLO_CORE_TRACE("Loaded asset: {}", metadata.FilePath.string());
        return asset;
    }

    void EditorAssetManager::UpdateDependencies(AssetHandle handle)
    {
        std::shared_lock lock(m_DependenciesMutex);
        auto it = m_Dependencies.find(handle);
        if (it != m_Dependencies.end())
        {
            for (AssetHandle dependent : it->second)
            {
                // Notify dependent assets that this asset has changed
                auto asset = GetAsset(dependent);
                if (asset)
                {
                    asset->OnDependencyUpdated(handle);
                }
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
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        OLO_CORE_INFO("File watcher thread stopped");
    }
#endif

} // namespace OloEngine
