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
        
        // Initialize project path early to ensure proper path resolution
        if (Project::GetActive())
        {
            m_ProjectPath = Project::GetProjectDirectory();
            OLO_CORE_INFO("EditorAssetManager: Project path initialized to {}", m_ProjectPath.string());
        }
        else
        {
            OLO_CORE_WARN("EditorAssetManager: No active project found during initialization");
        }
        
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
        // Start file watcher thread only if we have a valid project path
        if (!m_ProjectPath.empty())
        {
            m_FileWatcherThread = std::thread([this]() { FileWatcherThreadFunction(); });
        }
#endif
    }

    void EditorAssetManager::Shutdown() noexcept
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
        
        // Shutdown AssetImporter to release serializer resources
        AssetImporter::Shutdown();
    }

    AssetType EditorAssetManager::GetAssetType(AssetHandle assetHandle) const noexcept
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

        OLO_CORE_TRACE("Added memory asset: {}", (u64)asset->m_Handle);
    }

    bool EditorAssetManager::ReloadData(AssetHandle assetHandle)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::ReloadData");

        auto metadata = m_AssetRegistry.GetMetadata(assetHandle);
        if (!metadata.IsValid())
        {
            OLO_CORE_ERROR("Cannot reload asset {}: metadata not found", (u64)assetHandle);
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

        // Update LastWriteTime to prevent continuous reloads
        {
            std::error_code ec;
            auto absolutePath = m_ProjectPath / metadata.FilePath;
            metadata.LastWriteTime = std::filesystem::last_write_time(absolutePath, ec);
            if (!ec)
            {
                // Thread-safe update of the metadata in the registry
                std::lock_guard<std::shared_mutex> lock(m_RegistryMutex);
                m_AssetRegistry.UpdateMetadata(assetHandle, metadata);
                SerializeAssetRegistry(); // Persist the updated timestamp
            }
            else
            {
                OLO_CORE_WARN("Failed to update LastWriteTime for asset {}: {}", metadata.FilePath.string(), ec.message());
            }
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

        // Convert to absolute path before checking existence, handling already-absolute paths
        std::filesystem::path absolutePath = metadata.FilePath.is_absolute() ? metadata.FilePath : (m_ProjectPath / metadata.FilePath);
        
        // Check if file exists before checking modification time using error_code to avoid exceptions
        std::error_code ec;
        if (!std::filesystem::exists(absolutePath, ec))
        {
            if (ec)
            {
                OLO_CORE_WARN("Error checking asset file existence for {}: {}", metadata.FilePath.string(), ec.message());
            }
            else
            {
                OLO_CORE_WARN("Asset file does not exist: {}", absolutePath.string());
            }
            return false;
        }

        // Check if file has been modified using error_code to avoid exceptions
        auto lastWriteTime = std::filesystem::last_write_time(absolutePath, ec);
        if (ec)
        {
            OLO_CORE_WARN("Error getting last write time for {}: {}", absolutePath.string(), ec.message());
            return false;
        }
        
        if (lastWriteTime > metadata.LastWriteTime)
        {
            return ReloadData(assetHandle);
        }

        return true;
    }

    bool EditorAssetManager::EnsureAllLoadedCurrent()
    {
        // First, collect all asset handles to check
        std::vector<AssetHandle> assetHandles;
        {
            std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
            assetHandles.reserve(m_LoadedAssets.size());
            for (const auto& [handle, asset] : m_LoadedAssets)
            {
                assetHandles.push_back(handle);
            }
        }

        // Now check each asset without holding the mutex
        bool allCurrent = true;
        for (const auto& handle : assetHandles)
        {
            if (!EnsureCurrent(handle))
                allCurrent = false;
        }

        return allCurrent;
    }

    bool EditorAssetManager::IsAssetHandleValid(AssetHandle assetHandle) const noexcept
    {
        return m_AssetRegistry.Exists(assetHandle);
    }

    Ref<Asset> EditorAssetManager::GetMemoryAsset(AssetHandle handle) const
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        auto it = m_MemoryAssets.find(handle);
        return (it != m_MemoryAssets.end()) ? it->second : nullptr;
    }

    bool EditorAssetManager::IsAssetLoaded(AssetHandle handle) const noexcept
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end();
    }

    bool EditorAssetManager::IsAssetValid(AssetHandle handle) const noexcept
    {
        return IsAssetHandleValid(handle);
    }

    bool EditorAssetManager::IsAssetMissing(AssetHandle handle) const noexcept
    {
        auto metadata = m_AssetRegistry.GetMetadata(handle);
        if (!metadata.IsValid())
            return true;

        // Resolve file path to absolute path relative to project root
        std::filesystem::path absolutePath = m_ProjectPath / metadata.FilePath;
        
        // Use error_code overload to safely check existence without throwing exceptions
        std::error_code ec;
        bool exists = std::filesystem::exists(absolutePath, ec);
        
        if (ec)
        {
            OLO_CORE_WARN("Error checking asset existence for {}: {}", absolutePath.string(), ec.message());
            return true; // Assume missing if we can't check
        }
        
        return !exists;
    }

    bool EditorAssetManager::IsMemoryAsset(AssetHandle handle) const noexcept
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        return m_MemoryAssets.find(handle) != m_MemoryAssets.end();
    }

    bool EditorAssetManager::IsPhysicalAsset(AssetHandle handle) const noexcept
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

        OLO_CORE_TRACE("Removed asset: {}", (u64)handle);
    }

    void EditorAssetManager::RegisterDependency(AssetHandle handle, AssetHandle dependency)
    {
        std::unique_lock<std::shared_mutex> lock(m_DependenciesMutex);
        m_AssetDependencies[dependency].insert(handle);
    }

    void EditorAssetManager::DeregisterDependency(AssetHandle handle, AssetHandle dependency)
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

    std::unordered_set<AssetHandle> EditorAssetManager::GetDependencies(AssetHandle handle) const
    {
        std::shared_lock<std::shared_mutex> lock(m_DependenciesMutex);
        auto it = m_AssetDependencies.find(handle);
        return (it != m_AssetDependencies.end()) ? it->second : std::unordered_set<AssetHandle>{};
    }

    std::unordered_set<AssetHandle> EditorAssetManager::GetAllAssetsWithType(AssetType type) const
    {
        std::unordered_set<AssetHandle> result;
        
        // Check loaded assets
        {
            std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
            for (const auto& [handle, asset] : m_LoadedAssets)
            {
                if (asset && asset->GetAssetType() == type)
                    result.insert(handle);
            }
            
            // Check memory assets
            for (const auto& [handle, asset] : m_MemoryAssets)
            {
                if (asset && asset->GetAssetType() == type)
                    result.insert(handle);
            }
        }
        
        // Check asset registry metadata
        {
            std::shared_lock<std::shared_mutex> lock(m_RegistryMutex);
            auto registryHandles = m_AssetRegistry.GetAssetHandlesOfType(type);
            result.insert(registryHandles.begin(), registryHandles.end());
        }
        
        return result;
    }

    AssetHandle EditorAssetManager::ImportAsset(const std::filesystem::path& filepath)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::ImportAsset");

        // Normalize to absolute project path before checking existence
        std::filesystem::path absolutePath = std::filesystem::absolute(filepath);
        
        // Use error_code overload to handle filesystem errors gracefully
        std::error_code ec;
        if (!std::filesystem::exists(absolutePath, ec))
        {
            if (ec)
            {
                OLO_CORE_ERROR("Error checking file existence for {}: {}", filepath.string(), ec.message());
            }
            else
            {
                OLO_CORE_ERROR("Cannot import asset: file does not exist: {}", filepath.string());
            }
            return 0;
        }

        AssetType type = AssetExtensions::GetAssetTypeFromPath(filepath.string());
        if (type == AssetType::None)
        {
            OLO_CORE_ERROR("Cannot import asset: unsupported file type: {}", filepath.string());
            return 0;
        }

        // Convert to project-relative path
        std::filesystem::path relativePath = GetRelativePath(filepath);

        // Check if already imported
        AssetHandle existingHandle = m_AssetRegistry.GetHandleFromPath(relativePath);
        if (existingHandle != 0)
        {
            OLO_CORE_WARN("Asset already imported: {}", relativePath.string());
            return existingHandle;
        }

        // Create metadata
        AssetMetadata metadata;
        metadata.Handle = m_AssetRegistry.GenerateHandle(); // Generate a valid handle
        metadata.FilePath = relativePath;
        metadata.Type = type;
        
        // Use error_code overload to avoid exceptions (reuse existing ec variable)
        metadata.LastWriteTime = std::filesystem::last_write_time(filepath, ec);
        if (ec)
        {
            OLO_CORE_WARN("Failed to get last write time for asset {}: {}", filepath.string(), ec.message());
            metadata.LastWriteTime = std::filesystem::file_time_type{}; // Default/empty timestamp
        }

        // Register in registry
        m_AssetRegistry.AddAsset(metadata);

        OLO_CORE_INFO("Imported asset: {} -> {}", filepath.string(), (u64)metadata.Handle);
        return metadata.Handle;
    }

    void EditorAssetManager::SyncWithAssetThread() noexcept
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
            try
            {
                // Perform periodic modification time scan over registry entries
                std::vector<AssetHandle> modifiedAssets;
                
                {
                    std::shared_lock<std::shared_mutex> registryLock(m_RegistryMutex);
                    auto allAssets = m_AssetRegistry.GetAllAssets();
                    for (const auto& metadata : allAssets)
                    {
                        if (!metadata.IsValid())
                            continue;
                            
                        // Convert relative path back to absolute for filesystem operations, handling already-absolute paths
                        std::filesystem::path absolutePath = metadata.FilePath.is_absolute() ? metadata.FilePath : (m_ProjectPath / metadata.FilePath);
                        
                        // Use error_code overloads to avoid exceptions that could terminate the background thread
                        std::error_code ec;
                        if (std::filesystem::exists(absolutePath, ec) && !ec)
                        {
                            auto currentWriteTime = std::filesystem::last_write_time(absolutePath, ec);
                            if (!ec && currentWriteTime > metadata.LastWriteTime)
                            {
                                modifiedAssets.push_back(metadata.Handle);
                            }
                            else if (ec)
                            {
                                OLO_CORE_WARN("Failed to get last write time for asset {}: {}", absolutePath.string(), ec.message());
                            }
                        }
                        else if (ec)
                        {
                            OLO_CORE_WARN("Error checking asset file existence for {}: {}", absolutePath.string(), ec.message());
                        }
                    }
                }
                
                // Reload modified assets (outside of registry lock to avoid deadlock)
                for (AssetHandle handle : modifiedAssets)
                {
                    OLO_CORE_INFO("Detected file modification, reloading asset: {}", (u64)handle);
                    ReloadDataAsync(handle);
                }
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("Error in file watcher thread: {}", e.what());
            }
            
            // Sleep for 1 second between scans
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        OLO_CORE_INFO("File watcher thread stopped");
    }
#endif

    std::filesystem::path EditorAssetManager::GetRelativePath(const std::filesystem::path& filepath)
    {
        // If the project path is empty, return the filepath as-is
        if (m_ProjectPath.empty())
            return filepath;
            
        // Use weakly_canonical for robust path resolution with symlinks and ".." components
        auto canonicalFile = std::filesystem::weakly_canonical(filepath);
        auto canonicalProject = std::filesystem::weakly_canonical(m_ProjectPath);
        
        // Return relative path from project root
        return std::filesystem::relative(canonicalFile, canonicalProject);
    }

    std::unordered_map<AssetHandle, Ref<Asset>> EditorAssetManager::GetLoadedAssets() const
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        return m_LoadedAssets;
    }

    void EditorAssetManager::ForEachLoadedAsset(const std::function<bool(AssetHandle, const Ref<Asset>&)>& callback) const
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        for (const auto& [handle, asset] : m_LoadedAssets)
        {
            if (!callback(handle, asset))
                break;
        }
    }

    std::unordered_map<AssetHandle, Ref<Asset>> EditorAssetManager::GetLoadedAssetsCopy() const
    {
        std::shared_lock<std::shared_mutex> lock(m_AssetsMutex);
        return m_LoadedAssets;
    }

} // namespace OloEngine
