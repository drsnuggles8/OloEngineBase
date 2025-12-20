#include "EditorAssetManager.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include "OloEngine/Asset/PlaceholderAsset.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Core/Events/EditorEvents.h"

#include <algorithm>
#include <future>
#include <filesystem>

#if OLO_ASYNC_ASSETS
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4068) // unknown pragma 'mark'
#endif
#include "FileWatch.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif

namespace OloEngine
{
    EditorAssetManager::EditorAssetManager()
    {
#if OLO_ASYNC_ASSETS
        m_AssetThread = Ref<EditorAssetSystem>::Create();
#endif

        AssetImporter::Init();
        PlaceholderAssetManager::Initialize();
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

            // Scan project assets directory for any new assets that aren't in the registry
            auto assetDirectory = Project::GetAssetDirectory();
            if (std::filesystem::exists(assetDirectory, ec) && !ec)
            {
                OLO_CORE_INFO("Scanning asset directory for new assets: {}", assetDirectory.string());
                ScanDirectoryForAssets(assetDirectory);
                OLO_CORE_INFO("Asset directory scan completed");

                // Serialize the updated registry to save any newly discovered assets
                SerializeAssetRegistry();
            }
            else if (ec)
            {
                OLO_CORE_WARN("Failed to check asset directory existence: {}", ec.message());
            }
        }

#if OLO_ASYNC_ASSETS
        // Start real-time file watcher for the project directory
        if (!m_ProjectPath.empty())
        {
            OLO_CORE_INFO("Starting real-time file watcher for project: {}", m_ProjectPath.string());
            try
            {
                m_ProjectFileWatcher = std::make_unique<filewatch::FileWatch<std::string>>(
                    m_ProjectPath.string(),
                    [this](const std::string& file, const filewatch::Event change_type)
                    {
                        OnFileSystemEvent(file, change_type);
                    });
                OLO_CORE_INFO("Real-time file watcher started successfully");
            }
            catch (const std::exception& e)
            {
                OLO_CORE_ERROR("Failed to start file watcher: {}", e.what());
                OLO_CORE_INFO("Falling back to polling-based file watching using Task System");

                // Use Task System for polling-based file watching instead of dedicated thread
                m_ShouldTerminate.store(false, std::memory_order_relaxed);
                m_FileWatcherTaskActive.store(true, std::memory_order_release);
                u32 currentGeneration = m_FileWatcherGeneration.load(std::memory_order_relaxed);

                Tasks::Launch(
                    "EditorAssetManager_FileWatcher",
                    [this, currentGeneration]()
                    {
                        FileWatcherThreadFunction();
                        m_FileWatcherTaskActive.store(false, std::memory_order_release);
                    },
                    Tasks::ETaskPriority::BackgroundNormal);
            }
        }
#endif
    }

    void EditorAssetManager::Shutdown() noexcept
    {
#if OLO_ASYNC_ASSETS
        // Stop asset thread
        if (m_AssetThread)
        {
            m_AssetThread->StopAndWait();
        }

        // Stop real-time file watcher
        if (m_ProjectFileWatcher)
        {
            m_ProjectFileWatcher.reset();
        }

        // Stop polling file watcher (Task System fallback)
        m_ShouldTerminate.store(true, std::memory_order_release);
        m_FileWatcherGeneration.fetch_add(1, std::memory_order_relaxed); // Invalidate any active task

        // Wait for file watcher task to complete
        if (m_FileWatcherTaskActive.load(std::memory_order_acquire))
        {
            OLO_CORE_INFO("EditorAssetManager: Waiting for file watcher task to complete...");
            while (m_FileWatcherTaskActive.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }

        // Wait for any pending async reload tasks
        u32 remainingReloads = m_ActiveReloadTasks.load(std::memory_order_acquire);
        if (remainingReloads > 0)
        {
            OLO_CORE_INFO("EditorAssetManager: Waiting for {} async reload tasks to complete...", remainingReloads);
            while (m_ActiveReloadTasks.load(std::memory_order_acquire) > 0)
            {
                std::this_thread::yield();
            }
        }
#endif

        // Save asset registry
        if (Project::GetActive())
        {
            auto registryPath = Project::GetAssetRegistryPath();
            m_AssetRegistry.Serialize(registryPath);
        }

        // Clear all loaded assets and memory assets
        {
            std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
            m_LoadedAssets.clear();
            m_MemoryAssets.clear();
        }

        AssetImporter::Shutdown();
        PlaceholderAssetManager::Shutdown();
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

        // Capture metadata values under registry lock for thread safety
        AssetMetadata metadata;
        AssetType type;
        std::filesystem::path path;
        {
            std::shared_lock<std::shared_mutex> lock(m_RegistryMutex);
            metadata = m_AssetRegistry.GetMetadata(assetHandle);
            if (!metadata.IsValid())
            {
                OLO_CORE_ERROR("Cannot reload asset {}: metadata not found", (u64)assetHandle);
                return false;
            }

            // Capture values while under lock to avoid accessing stale metadata later
            type = metadata.Type;
            path = metadata.FilePath;
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
            OLO_CORE_ERROR("Failed to reload asset: {}", path.string());

            // Load a safe placeholder asset instead of failing completely
            auto placeholderAsset = AssetManager::GetPlaceholderAsset(type);
            if (placeholderAsset)
            {
                // Set the placeholder's handle to match the original asset handle
                placeholderAsset->SetHandle(assetHandle);

                // Cache the placeholder asset so callers get a valid asset reference
                {
                    std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
                    m_LoadedAssets[assetHandle] = placeholderAsset;
                }

                // Set status to Failed (we could add a new FailedWithPlaceholder status in the future)
                SetAssetStatus(assetHandle, AssetStatus::Failed);

                OLO_CORE_WARN("Asset reload failed, substituted with placeholder: {} -> {} (Type: {})",
                              path.string(), (u64)assetHandle, AssetUtils::AssetTypeToString(type));

                // Continue with normal workflow even with placeholder
                asset = placeholderAsset;
            }
            else
            {
                OLO_CORE_ERROR("Failed to create placeholder asset for type: {}", AssetUtils::AssetTypeToString(type));
                SetAssetStatus(assetHandle, AssetStatus::Failed);
                return false;
            }
        }

        // Update LastWriteTime to prevent continuous reloads
        {
            std::error_code ec;
            auto absolutePath = m_ProjectPath / path;
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
                OLO_CORE_WARN("Failed to update LastWriteTime for asset {}: {}", path.string(), ec.message());
            }
        }

        // Update dependencies
        UpdateDependencies(assetHandle);

        // Notify dependent assets that this asset has been updated
        UpdateDependents(assetHandle);

        // Notify listeners via engine event system (on main thread)
        {
            Application::Get().SubmitToMainThread([assetHandle, type, path]() mutable
                                                  {
                AssetReloadedEvent evt(assetHandle, type, path);
                Application::Get().OnEvent(evt); });
        }

        OLO_CORE_INFO("Reloaded asset: {}", path.string());
        return true;
    }

    void EditorAssetManager::ReloadDataAsync(AssetHandle assetHandle)
    {
#if OLO_ASYNC_ASSETS
        // Use Task System for proper async reloading
        // Note: ReloadData contains OpenGL operations via asset loading, so we must
        // submit the actual reload to the main thread. The "async" part is that we
        // don't block the caller - the reload happens later on the main thread.
        m_ActiveReloadTasks.fetch_add(1, std::memory_order_relaxed);

        Application::Get().SubmitToMainThread(
            [this, assetHandle]()
            {
                ReloadData(assetHandle);
                m_ActiveReloadTasks.fetch_sub(1, std::memory_order_relaxed);
            });
#else
        // Synchronous fallback when async assets are disabled
        ReloadData(assetHandle);
#endif
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

        OLO_CORE_ASSERT(handle != 0, "Cannot register dependency for invalid asset handle");

        // Ensure there is an entry in m_AssetDependencies for handle (creates if needed)
        auto& dependencySet = m_AssetDependencies[handle];

        // Return early if dependency is invalid
        if (dependency == 0)
            return;

        // Asset 'handle' depends on 'dependency'
        dependencySet.insert(dependency);
        // Asset 'dependency' is depended on by 'handle'
        m_AssetDependents[dependency].insert(handle);
    }

    void EditorAssetManager::DeregisterDependency(AssetHandle handle, AssetHandle dependency)
    {
        std::unique_lock<std::shared_mutex> lock(m_DependenciesMutex);
        if (dependency != 0)
        {
            // Remove 'dependency' from what 'handle' depends on
            m_AssetDependencies[handle].erase(dependency);
            // Remove 'handle' from what depends on 'dependency'
            m_AssetDependents[dependency].erase(handle);
        }
    }

    void EditorAssetManager::DeregisterDependencies(AssetHandle handle)
    {
        std::unique_lock<std::shared_mutex> lock(m_DependenciesMutex);

        // Find all dependencies this asset has
        if (auto it = m_AssetDependencies.find(handle); it != m_AssetDependencies.end())
        {
            // For each dependency, remove this asset from its dependents list
            for (AssetHandle dependency : it->second)
            {
                m_AssetDependents[dependency].erase(handle);
            }
            // Remove this asset's dependency list
            m_AssetDependencies.erase(it);
        }

        // Also remove this asset from being a dependent of anything
        // (This handles cases where this asset was incorrectly registered)
        for (auto& [depHandle, dependents] : m_AssetDependents)
        {
            dependents.erase(handle);
        }
    }

    void EditorAssetManager::UpdateDependents(AssetHandle handle)
    {
        std::unordered_set<AssetHandle> dependents;
        {
            std::shared_lock lock(m_DependenciesMutex);
            if (auto it = m_AssetDependents.find(handle); it != m_AssetDependents.end())
                dependents = it->second;
        }

        for (AssetHandle dependent : dependents)
        {
            if (IsAssetLoaded(dependent))
            {
                Ref<Asset> asset = GetAsset(dependent);
                if (asset)
                {
                    OLO_CORE_TRACE("Notifying dependent asset {} of dependency {} update",
                                   (u64)dependent, (u64)handle);
                    asset->OnDependencyUpdated(handle);
                }
            }
        }
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
#if OLO_ASYNC_ASSETS
        if (!m_AssetThread)
            return;

        OLO_PROFILER_SCOPE("EditorAssetManager::SyncWithAssetThread");

        // Retrieve ready assets from the asset thread
        std::vector<EditorAssetLoadResponse> freshAssets;
        if (!m_AssetThread->RetrieveReadyAssets(freshAssets))
            return; // No new assets

        // Integrate ready assets into the main asset manager
        {
            std::unique_lock<std::shared_mutex> lock(m_AssetsMutex);
            for (const auto& response : freshAssets)
            {
                if (response.AssetRef)
                {
                    m_LoadedAssets[response.Metadata.Handle] = response.AssetRef;
                    OLO_CORE_TRACE("SyncWithAssetThread: Integrated asset {} from async load",
                                   (u64)response.Metadata.Handle);

                    // Update asset status to Loaded in registry
                    {
                        std::unique_lock<std::shared_mutex> registryLock(m_RegistryMutex);
                        auto metadata = m_AssetRegistry.GetMetadata(response.Metadata.Handle);
                        if (metadata.IsValid())
                        {
                            metadata.Status = AssetStatus::Loaded;
                            m_AssetRegistry.UpdateMetadata(response.Metadata.Handle, metadata);
                        }
                    }

                    // TODO: Fire AssetLoadedEvent for listeners
                }
            }
        }

        OLO_CORE_TRACE("SyncWithAssetThread: Integrated {} assets from async loading", freshAssets.size());

        // Log telemetry information
        if (freshAssets.size() > 0)
        {
            auto [queued, loaded, failed, queueLength] = m_AssetThread->GetTelemetry();
            OLO_CORE_TRACE("Asset Thread Telemetry - Queued: {}, Loaded: {}, Failed: {}, Queue Length: {}",
                           queued, loaded, failed, queueLength);
        }
#else
        // In synchronous mode, this is a no-op
#endif
    }

    Ref<Asset> EditorAssetManager::LoadAssetFromFile(const AssetMetadata& metadata)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::LoadAsset");

        if (!metadata.IsValid())
        {
            OLO_CORE_ERROR("Cannot load asset: invalid metadata");
            SetAssetStatus(metadata.Handle, AssetStatus::Invalid);
            return AssetManager::GetPlaceholderAsset(metadata.Type);
        }

        auto absolutePath = m_ProjectPath / metadata.FilePath;
        if (!std::filesystem::exists(absolutePath))
        {
            OLO_CORE_ERROR("Cannot load asset: file does not exist: {}", metadata.FilePath.string());
            SetAssetStatus(metadata.Handle, AssetStatus::Missing);
            return AssetManager::GetPlaceholderAsset(metadata.Type);
        }

        // Set loading status
        SetAssetStatus(metadata.Handle, AssetStatus::Loading);

        // Load asset using importer
        Ref<Asset> asset;
        if (!AssetImporter::TryLoadData(metadata, asset))
        {
            OLO_CORE_ERROR("Failed to load asset: {}", metadata.FilePath.string());
            SetAssetStatus(metadata.Handle, AssetStatus::Failed);
            return AssetManager::GetPlaceholderAsset(metadata.Type);
        }

        // Successfully loaded
        SetAssetStatus(metadata.Handle, AssetStatus::Loaded);

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

#if OLO_ASYNC_ASSETS
    void EditorAssetManager::OnFileSystemEvent(const std::string& file, const filewatch::Event change_type)
    {
        // Convert to filesystem path for easier manipulation
        std::filesystem::path filePath(file);

        // Only handle modification events (ignoring added/removed for now)
        if (change_type != filewatch::Event::modified)
            return;

        // Filter by asset extensions to avoid processing non-asset files
        auto extension = filePath.extension();
        if (extension.empty())
            return;

        auto assetType = AssetExtensions::GetAssetTypeFromExtension(extension.string());
        if (assetType == AssetType::None)
        {
            OLO_CORE_TRACE("Ignoring file change for non-asset file: {}", filePath.string());
            return;
        }

        OLO_CORE_TRACE("File system event: {} - {} (AssetType: {})", file, (int)change_type, (int)assetType);

        try
        {
            // The file watcher gives us paths relative to the project root
            // So we can use the path directly for asset registry lookup
            std::filesystem::path assetPath = filePath;

            // Normalize the path separators to match asset registry format
            std::string pathStr = assetPath.generic_string();

            // Find the asset handle for this file
            AssetHandle assetHandle = 0;
            {
                std::shared_lock<std::shared_mutex> registryLock(m_RegistryMutex);
                auto allAssets = m_AssetRegistry.GetAllAssets();
                for (const auto& metadata : allAssets)
                {
                    if (!metadata.IsValid())
                        continue;

                    // Compare normalized paths as strings (case insensitive for Windows)
                    std::string registryPath = metadata.FilePath.generic_string();

                    // Windows is case-insensitive, so compare lowercase
                    std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(), ::tolower);
                    std::transform(registryPath.begin(), registryPath.end(), registryPath.begin(), ::tolower);

                    if (pathStr == registryPath)
                    {
                        assetHandle = metadata.Handle;
                        break;
                    }
                }
            }

            // If we found the asset, reload it
            if (assetHandle != 0)
            {
                OLO_CORE_INFO("🔄 Hot-reload triggered for asset: {} (Handle: {}, Type: {})",
                              pathStr, (u64)assetHandle, (int)assetType);
                ReloadDataAsync(assetHandle);
            }
            else
            {
                // Check if this might be a new asset file
                OLO_CORE_TRACE("File change detected for untracked file: {} (Type: {})",
                               pathStr, (int)assetType);
                // TODO: In the future, we could auto-import new assets here
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Error handling file system event for {}: {}", file, e.what());
        }
    }
#endif

    AssetMetadata EditorAssetManager::GetMetadata(AssetHandle handle) const
    {
        // Return by value to prevent dangling reference after lock is released.
        // Previously returned const reference while holding shared_lock, but the lock
        // was released when the function returned (end of scope), creating a race where:
        // 1. Thread A gets reference to metadata
        // 2. Lock is released when function returns
        // 3. Thread B removes the asset
        // 4. Thread A's reference is now dangling
        std::shared_lock lock(m_RegistryMutex);
        return m_AssetRegistry.GetMetadata(handle);
    }

    void EditorAssetManager::SetMetadata(AssetHandle handle, const AssetMetadata& metadata)
    {
        std::unique_lock lock(m_RegistryMutex);
        m_AssetRegistry.UpdateMetadata(handle, metadata);
    }

    void EditorAssetManager::SetAssetStatus(AssetHandle handle, AssetStatus status)
    {
        std::unique_lock lock(m_RegistryMutex);
        auto metadata = m_AssetRegistry.GetMetadata(handle);
        if (metadata.IsValid())
        {
            metadata.Status = status;
            m_AssetRegistry.UpdateMetadata(handle, metadata);
        }
    }

    void EditorAssetManager::ScanDirectoryForAssets(const std::filesystem::path& directory)
    {
        OLO_PROFILER_SCOPE("EditorAssetManager::ScanDirectoryForAssets");

        if (!std::filesystem::exists(directory))
        {
            OLO_CORE_WARN("Directory does not exist for asset scanning: {}", directory.string());
            return;
        }

        std::error_code ec;
        auto iterator = std::filesystem::recursive_directory_iterator(directory, ec);
        if (ec)
        {
            OLO_CORE_ERROR("Failed to create recursive directory iterator for {}: {}", directory.string(), ec.message());
            return;
        }

        for (auto it = iterator; it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                OLO_CORE_WARN("Error advancing directory iterator during asset scan: {}", ec.message());
                ec.clear(); // Clear error and continue with next iteration
                continue;
            }

            try
            {
                const auto& entry = *it;

                // Check if it's a regular file using error_code overload
                std::error_code file_ec;
                bool isRegularFile = entry.is_regular_file(file_ec);
                if (file_ec)
                {
                    OLO_CORE_WARN("Error checking file type for {}: {}", entry.path().string(), file_ec.message());
                    continue; // Skip this entry and continue
                }

                if (isRegularFile)
                {
                    // Get the path safely
                    std::filesystem::path entryPath;
                    try
                    {
                        entryPath = entry.path();
                    }
                    catch (const std::filesystem::filesystem_error& path_ex)
                    {
                        OLO_CORE_WARN("Error getting path for directory entry: {}", path_ex.what());
                        continue; // Skip this entry and continue
                    }

                    AssetType type = AssetExtensions::GetAssetTypeFromPath(entryPath.string());
                    if (type != AssetType::None)
                    {
                        ImportAsset(entryPath);
                    }
                }
            }
            catch (const std::filesystem::filesystem_error& ex)
            {
                OLO_CORE_WARN("Filesystem error processing directory entry during asset scan: {}", ex.what());
                // Continue to next entry
                continue;
            }
            catch (const std::exception& ex)
            {
                OLO_CORE_WARN("Unexpected error processing directory entry during asset scan: {}", ex.what());
                // Continue to next entry
                continue;
            }
        }
    }

    bool EditorAssetManager::SerializeAssetRegistry()
    {
        try
        {
            std::scoped_lock lock(m_RegistryMutex);

            const std::filesystem::path registryPath = Project::GetAssetRegistryPath();
            return m_AssetRegistry.Serialize(registryPath);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to serialize asset registry: {}", e.what());
            return false;
        }
    }

} // namespace OloEngine
