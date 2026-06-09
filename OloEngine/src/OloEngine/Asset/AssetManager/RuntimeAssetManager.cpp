#include "OloEnginePCH.h"
#include "RuntimeAssetManager.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Events/EditorEvents.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Threading/UniqueLock.h"
#include "OloEngine/Threading/SharedLock.h"

#include <filesystem>
#include <vector>

namespace OloEngine
{
    RuntimeAssetManager::RuntimeAssetManager(bool autoLoadDefaultPack)
    {
#if OLO_ASYNC_ASSETS
        m_AssetThread = Ref<RuntimeAssetSystem>::Create(this);
#endif

        AssetImporter::Init();
        OLO_CORE_INFO("RuntimeAssetManager initialized");

        if (!autoLoadDefaultPack)
            return;

        // Load default asset pack if it exists
        const std::string assetPackPath = "Assets/AssetPack.olopack";
        std::error_code ec;
        if (std::filesystem::exists(assetPackPath, ec) && !ec)
        {
            if (!LoadAssetPack(assetPackPath))
            {
                OLO_CORE_WARN("Failed to load default asset pack: {}", assetPackPath);
            }
        }
        else if (ec)
        {
            OLO_CORE_WARN("Failed to check asset pack existence: {}", ec.message());
        }
        else
        {
            OLO_CORE_INFO("Default asset pack not found: {}", assetPackPath);
        }
    }

    RuntimeAssetManager::~RuntimeAssetManager()
    {
        Shutdown();
    }

    void RuntimeAssetManager::Shutdown() noexcept
    {
        OLO_CORE_INFO("Shutting down RuntimeAssetManager");

#if OLO_ASYNC_ASSETS
        // Stop asset thread
        if (m_AssetThread)
        {
            m_AssetThread->StopAndWait();
        }
#endif

        // Lock all mutexes atomically to prevent race conditions
        TUniqueLock<FSharedMutex> assetsLock(m_AssetsMutex);
        TUniqueLock<FSharedMutex> packsLock(m_PacksMutex);
        TUniqueLock<FSharedMutex> depsLock(m_DependenciesMutex);
        m_LoadedAssets.clear();
        m_MemoryAssets.clear();
        m_LoadedPacks.clear();
        m_AssetMetadata.clear();
        m_AssetDependencies.clear();

        // Shutdown AssetImporter to release serializer resources
        AssetImporter::Shutdown();
    }

    AssetType RuntimeAssetManager::GetAssetType(AssetHandle assetHandle) const noexcept
    {
        // Check loaded assets and memory assets under a single shared lock
        {
            TSharedLock<FSharedMutex> lock(m_AssetsMutex);

            // Check loaded assets first
            if (auto it = m_LoadedAssets.find(assetHandle); it != m_LoadedAssets.end() && it->second)
            {
                return it->second->GetAssetType();
            }

            // Check memory assets
            if (auto memIt = m_MemoryAssets.find(assetHandle); memIt != m_MemoryAssets.end() && memIt->second)
            {
                return memIt->second->GetAssetType();
            }
        }

        // Check asset pack metadata
        return GetAssetTypeFromPacks(assetHandle);
    }

    AssetMetadata RuntimeAssetManager::GetAssetMetadata(AssetHandle handle) const noexcept
    {
        TSharedLock<FSharedMutex> lock(m_PacksMutex);

        // Return copy of metadata from the loaded packs storage
        if (auto it = m_AssetMetadata.find(handle); it != m_AssetMetadata.end())
        {
            return it->second; // Return copy
        }

        // Return empty metadata if not found
        return AssetMetadata{};
    }

    Ref<Asset> RuntimeAssetManager::GetAsset(AssetHandle assetHandle)
    {
        if (assetHandle == 0)
            return nullptr;

        // First check: acquire shared lock and check if asset is already loaded
        {
            TSharedLock<FSharedMutex> lock(m_AssetsMutex);

            // Check loaded assets first
            if (auto it = m_LoadedAssets.find(assetHandle); it != m_LoadedAssets.end())
                return it->second;

            // Check memory assets
            if (auto memIt = m_MemoryAssets.find(assetHandle); memIt != m_MemoryAssets.end())
                return memIt->second;
        }

        // Asset not found, need to load it - acquire unique lock for double-checked locking
        {
            TDynamicUniqueLock<FSharedMutex> lock(m_AssetsMutex);

            // Second check: verify asset wasn't loaded by another thread while we waited for the lock
            if (auto it = m_LoadedAssets.find(assetHandle); it != m_LoadedAssets.end())
                return it->second;

            // Check memory assets again
            if (auto memIt = m_MemoryAssets.find(assetHandle); memIt != m_MemoryAssets.end())
                return memIt->second;

            // Asset still not loaded, safe to load it now
            // Release the lock temporarily while loading from pack (expensive operation)
            lock.Unlock();
            Ref<Asset> asset = LoadAssetFromPack(assetHandle);

            if (asset)
            {
                // Re-acquire lock to insert the loaded asset
                lock.Lock();

                // Final check: ensure no other thread loaded it while we were loading
                if (auto finalIt = m_LoadedAssets.find(assetHandle); finalIt != m_LoadedAssets.end())
                {
                    // Another thread loaded it, return that instance
                    return finalIt->second;
                }

                // Safe to insert our loaded asset
                m_LoadedAssets[assetHandle] = asset;
                return asset;
            }
        }

        return nullptr;
    }

    AsyncAssetResult<Asset> RuntimeAssetManager::GetAssetAsync(AssetHandle assetHandle)
    {
        if (assetHandle == 0)
            return AsyncAssetResult<Asset>{ nullptr, false };

        // Fast path: already loaded or memory-only asset
        {
            TSharedLock<FSharedMutex> lock(m_AssetsMutex);
            if (auto it = m_LoadedAssets.find(assetHandle); it != m_LoadedAssets.end())
                return AsyncAssetResult<Asset>{ it->second, true };
            if (auto memIt = m_MemoryAssets.find(assetHandle); memIt != m_MemoryAssets.end())
                return AsyncAssetResult<Asset>{ memIt->second, true };
        }

#if OLO_ASYNC_ASSETS
        // Queue for background loading only when the asset lives in a pack and its
        // serializer is safe to run off the main thread (no GPU work). GPU-backed types
        // (textures, meshes, ...) and unknown types fall through to synchronous loading.
        if (m_AssetThread)
        {
            AssetType type = GetAssetTypeFromPacks(assetHandle);
            if (type != AssetType::None && AssetImporter::CanDeserializeFromAssetPackOffThread(type))
            {
                m_AssetThread->QueueAssetLoad(RuntimeAssetLoadRequest(0, assetHandle));

                // Not ready yet; caller should call SyncWithAssetThread() and retry.
                return AsyncAssetResult<Asset>{ nullptr, false };
            }
        }
#endif

        // Fallback: synchronous load (GPU types, or no async thread available).
        Ref<Asset> asset = GetAsset(assetHandle);
        return AsyncAssetResult<Asset>{ asset, asset != nullptr };
    }

    void RuntimeAssetManager::AddMemoryOnlyAsset(Ref<Asset> asset)
    {
        if (!asset || asset->m_Handle == 0)
            return;

        TUniqueLock<FSharedMutex> lock(m_AssetsMutex);
        m_MemoryAssets[asset->m_Handle] = asset;
    }

    bool RuntimeAssetManager::ReloadData([[maybe_unused]] AssetHandle assetHandle)
    {
        // Runtime manager doesn't support reloading from individual files
        // Assets are loaded from packs which don't change at runtime
        OLO_CORE_WARN("RuntimeAssetManager::ReloadData - Reloading not supported in runtime mode");
        return false;
    }

    void RuntimeAssetManager::ReloadDataAsync([[maybe_unused]] AssetHandle assetHandle)
    {
        // Runtime manager doesn't support async reloading
        OLO_CORE_WARN("RuntimeAssetManager::ReloadDataAsync - Async reloading not supported in runtime mode");
    }

    bool RuntimeAssetManager::EnsureCurrent(AssetHandle assetHandle)
    {
        // In runtime mode, assets are always current (loaded from static packs)
        return IsAssetValid(assetHandle);
    }

    bool RuntimeAssetManager::EnsureAllLoadedCurrent()
    {
        // In runtime mode, all assets are always current
        return true;
    }

    bool RuntimeAssetManager::IsAssetHandleValid(AssetHandle assetHandle) const noexcept
    {
        if (assetHandle == 0)
            return false;

        // Check if it's a memory asset
        if (IsMemoryAsset(assetHandle))
            return true;

        // Check if it exists in any loaded pack
        return AssetExistsInPacks(assetHandle);
    }

    Ref<Asset> RuntimeAssetManager::GetMemoryAsset(AssetHandle handle) const
    {
        TSharedLock<FSharedMutex> lock(m_AssetsMutex);
        auto it = m_MemoryAssets.find(handle);
        return (it != m_MemoryAssets.end()) ? it->second : nullptr;
    }

    bool RuntimeAssetManager::IsAssetLoaded(AssetHandle handle) const noexcept
    {
        TSharedLock<FSharedMutex> lock(m_AssetsMutex);
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end();
    }

    bool RuntimeAssetManager::IsAssetValid(AssetHandle handle) const noexcept
    {
        // Check if it's loaded and valid
        {
            TSharedLock<FSharedMutex> lock(m_AssetsMutex);
            auto it = m_LoadedAssets.find(handle);
            if (it != m_LoadedAssets.end())
                return it->second != nullptr;
        }

        // Check if it's a valid memory asset
        if (IsMemoryAsset(handle))
            return GetMemoryAsset(handle) != nullptr;

        // Check if it exists in packs (doesn't load it)
        return AssetExistsInPacks(handle);
    }

    bool RuntimeAssetManager::IsAssetMissing(AssetHandle handle) const noexcept
    {
        // Memory assets cannot be missing
        if (IsMemoryAsset(handle))
            return false;

        // Check if asset exists in any pack
        return !AssetExistsInPacks(handle);
    }

    bool RuntimeAssetManager::IsMemoryAsset(AssetHandle handle) const noexcept
    {
        TSharedLock<FSharedMutex> lock(m_AssetsMutex);
        return m_MemoryAssets.find(handle) != m_MemoryAssets.end();
    }

    bool RuntimeAssetManager::IsPhysicalAsset(AssetHandle handle) const noexcept
    {
        return !IsMemoryAsset(handle) && AssetExistsInPacks(handle);
    }

    void RuntimeAssetManager::RemoveAsset(AssetHandle handle)
    {
        TUniqueLock<FSharedMutex> lock(m_AssetsMutex);

        // Remove from loaded assets
        m_LoadedAssets.erase(handle);

        // Remove from memory assets
        m_MemoryAssets.erase(handle);

        // Remove dependencies
        DeregisterDependencies(handle);

        // Remove stale generation counter
        ResetAssetGeneration(handle);
    }

    void RuntimeAssetManager::RegisterDependency(AssetHandle handle, AssetHandle dependency)
    {
        TUniqueLock<FSharedMutex> lock(m_DependenciesMutex);
        m_AssetDependencies[handle].insert(dependency);
    }

    void RuntimeAssetManager::DeregisterDependency(AssetHandle handle, AssetHandle dependency)
    {
        TUniqueLock<FSharedMutex> lock(m_DependenciesMutex);
        auto it = m_AssetDependencies.find(handle);
        if (it != m_AssetDependencies.end())
        {
            it->second.erase(dependency);
            if (it->second.empty())
                m_AssetDependencies.erase(it);
        }
    }

    void RuntimeAssetManager::DeregisterDependencies(AssetHandle handle)
    {
        TUniqueLock<FSharedMutex> lock(m_DependenciesMutex);
        m_AssetDependencies.erase(handle);
    }

    std::unordered_set<AssetHandle> RuntimeAssetManager::GetDependencies(AssetHandle handle) const
    {
        TSharedLock<FSharedMutex> lock(m_DependenciesMutex);
        auto it = m_AssetDependencies.find(handle);
        return (it != m_AssetDependencies.end()) ? it->second : std::unordered_set<AssetHandle>{};
    }

    void RuntimeAssetManager::SyncWithAssetThread() noexcept
    {
#if OLO_ASYNC_ASSETS
        if (!m_AssetThread)
            return;

        OLO_PROFILER_SCOPE("RuntimeAssetManager::SyncWithAssetThread");

        TArray<FCompletedAssetLoad> completed;
        if (!m_AssetThread->RetrieveCompletedAssets(completed))
            return;

        // Announce integrated assets after releasing the lock so handlers can call back
        // into the manager without deadlocking.
        struct LoadedEvent
        {
            AssetHandle Handle;
            AssetType Type;
        };
        std::vector<LoadedEvent> loadedEvents;

        {
            TUniqueLock<FSharedMutex> lock(m_AssetsMutex);
            for (const auto& result : completed)
            {
                if (!result.LoadedAsset)
                {
                    OLO_CORE_ERROR("RuntimeAssetManager::SyncWithAssetThread - Async load failed for asset {}", result.Handle);
                    continue;
                }

                // Drop completions whose handle is no longer provided by a loaded pack:
                // an UnloadAssetPack() between queue and completion already evicted it
                // (and rebuilt the metadata index), so re-inserting here would resurrect
                // a handle IsAssetMissing()/IsAssetHandleValid() now report as gone.
                // AssetExistsInPacks acquires m_PacksMutex (shared); the m_AssetsMutex ->
                // m_PacksMutex order matches UnloadAssetPack()/Shutdown(), so holding
                // m_AssetsMutex here serialises against the eviction.
                if (!AssetExistsInPacks(result.Handle))
                {
                    OLO_CORE_TRACE("RuntimeAssetManager::SyncWithAssetThread - Dropping stale async result for asset {} (pack unloaded before completion)", result.Handle);
                    continue;
                }

                m_LoadedAssets[result.Handle] = result.LoadedAsset;
                loadedEvents.push_back({ result.Handle, result.LoadedAsset->GetAssetType() });
                OLO_CORE_TRACE("RuntimeAssetManager::SyncWithAssetThread - Integrated async-loaded asset {}", result.Handle);
            }
        }

        if (!loadedEvents.empty())
        {
            if (Application* app = Application::TryGet())
            {
                for (const auto& evt : loadedEvents)
                {
                    AssetLoadedEvent loadedEvent(evt.Handle, evt.Type, std::filesystem::path{});
                    app->OnEvent(loadedEvent);
                }
            }
        }
#endif
    }

    std::unordered_set<AssetHandle> RuntimeAssetManager::GetAllAssetsWithType(AssetType type) const
    {
        std::unordered_set<AssetHandle> result;

        // Check loaded assets
        {
            TSharedLock<FSharedMutex> lock(m_AssetsMutex);
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

        // Check asset pack metadata
        {
            TSharedLock<FSharedMutex> lock(m_PacksMutex);
            for (const auto& [handle, metadata] : m_AssetMetadata)
            {
                if (metadata.Type == type)
                    result.insert(handle);
            }
        }

        return result;
    }

    std::unordered_map<AssetHandle, Ref<Asset>> RuntimeAssetManager::GetLoadedAssets() const
    {
        TSharedLock<FSharedMutex> lock(m_AssetsMutex);
        return m_LoadedAssets;
    }

    void RuntimeAssetManager::ForEachLoadedAsset(const std::function<bool(AssetHandle, const Ref<Asset>&)>& callback) const
    {
        TSharedLock<FSharedMutex> lock(m_AssetsMutex);
        for (const auto& [handle, asset] : m_LoadedAssets)
        {
            if (!callback(handle, asset))
                break;
        }
    }

    bool RuntimeAssetManager::LoadAssetPack(const std::filesystem::path& packPath)
    {
        if (!std::filesystem::exists(packPath))
        {
            OLO_CORE_ERROR("Asset pack not found: {}", packPath.string());
            return false;
        }

        // Check if already loaded
        {
            TSharedLock<FSharedMutex> lock(m_PacksMutex);
            if (m_LoadedPacks.find(packPath) != m_LoadedPacks.end())
            {
                OLO_CORE_WARN("Asset pack already loaded: {}", packPath.string());
                return true;
            }
        }

        // Create and load asset pack
        auto assetPack = Ref<AssetPack>::Create();
        if (!assetPack->Load(packPath))
        {
            OLO_CORE_ERROR("Failed to load asset pack: {}", packPath.string());
            return false;
        }

        // Store the loaded pack and index its assets into the metadata table.
        // m_AssetMetadata is the source of truth for every metadata-keyed query
        // (GetAsset, IsAssetValid, GetAssetType, GetAllAssetsWithType, ...); without
        // this step the loaded pack stays invisible and every asset lookup fails.
        {
            TUniqueLock<FSharedMutex> lock(m_PacksMutex);
            m_LoadedPacks[packPath] = assetPack;
            IndexAssetPackMetadata(*assetPack);
        }

        OLO_CORE_INFO("Loaded asset pack: {}", packPath.string());
        return true;
    }

    void RuntimeAssetManager::UnloadAssetPack(const std::filesystem::path& packPath)
    {
        // Lock order matches Shutdown(): m_AssetsMutex before m_PacksMutex. Both are
        // needed because unloading rebuilds the pack metadata index (m_PacksMutex)
        // and prunes the loaded-asset cache to match it (m_AssetsMutex).
        TUniqueLock<FSharedMutex> assetsLock(m_AssetsMutex);
        TUniqueLock<FSharedMutex> packsLock(m_PacksMutex);

        auto it = m_LoadedPacks.find(packPath);
        if (it == m_LoadedPacks.end())
            return;

        it->second->Unload();
        m_LoadedPacks.erase(it);

        // Rebuild the metadata index from the packs that remain loaded so this
        // pack's assets stop resolving, while assets still provided by another
        // loaded pack stay valid.
        m_AssetMetadata.clear();
        for (const auto& [remainingPath, remainingPack] : m_LoadedPacks)
            IndexAssetPackMetadata(*remainingPack);

        // Evict cached pack-backed assets the rebuilt index no longer covers, so
        // GetAsset() cannot return a handle that IsAssetHandleValid() / IsAssetMissing()
        // now report as gone. Memory-only assets (m_MemoryAssets) are not pack-backed
        // and are intentionally left untouched.
        std::erase_if(m_LoadedAssets, [this](const auto& entry)
                      { return m_AssetMetadata.find(entry.first) == m_AssetMetadata.end(); });

        OLO_CORE_INFO("Unloaded asset pack: {}", packPath.string());
    }

    AssetMetadata RuntimeAssetManager::GetAssetMetadataFromPacks(AssetHandle handle)
    {
        TSharedLock<FSharedMutex> lock(m_PacksMutex);
        auto it = m_AssetMetadata.find(handle);
        return (it != m_AssetMetadata.end()) ? it->second : AssetMetadata{};
    }

    void RuntimeAssetManager::UpdateDependencies(AssetHandle handle)
    {
        // For runtime, dependency updates are minimal
        // Dependencies are pre-resolved during pack creation

        // Notify the asset that a dependency was updated
        auto asset = GetAsset(handle);
        if (asset)
        {
            asset->OnDependencyUpdated(handle);
        }
    }

    Ref<Asset> RuntimeAssetManager::LoadAssetFromPack(AssetHandle handle)
    {
        // m_AssetMetadata and m_LoadedPacks are both guarded by m_PacksMutex; hold a
        // single shared lock across the metadata check and the pack scan so the view
        // stays consistent with a concurrent Load/Unload.
        TSharedLock<FSharedMutex> lock(m_PacksMutex);

        // Check if we have metadata for this asset
        auto metaIt = m_AssetMetadata.find(handle);
        if (metaIt == m_AssetMetadata.end())
        {
            OLO_CORE_ERROR("RuntimeAssetManager::LoadAssetFromPack - No metadata found for asset: {}", handle);
            return nullptr;
        }

        // Scenes are stored in a dedicated SceneInfo table; their entry in the regular
        // AssetInfo table is a type-only record whose PackedOffset is never populated by
        // the builder. Routing a scene through the AssetInfo path would seek to offset 0
        // (the file header) and read garbage, so dispatch scenes to the scene path.
        const bool isScene = (metaIt->second.Type == AssetType::Scene);

        // Find which pack contains the asset
        for (const auto& [packPath, assetPack] : m_LoadedPacks)
        {
            Ref<Asset> asset;

            if (isScene)
            {
                auto sceneInfo = assetPack->GetSceneInfo(handle);
                if (!sceneInfo.has_value())
                    continue;

                auto stream = assetPack->GetAssetStreamReader();
                if (!stream)
                    continue;

                asset = AssetImporter::DeserializeSceneFromAssetPack(*stream, sceneInfo.value());
            }
            else
            {
                if (!assetPack->IsAssetAvailable(handle))
                    continue;

                auto assetInfo = assetPack->GetAssetInfo(handle);
                if (!assetInfo.has_value())
                    continue;

                auto stream = assetPack->GetAssetStreamReader();
                if (!stream)
                    continue;

                asset = AssetImporter::DeserializeFromAssetPack(*stream, assetInfo.value());
            }

            if (asset)
            {
                asset->m_Handle = handle;
                OLO_CORE_TRACE("RuntimeAssetManager::LoadAssetFromPack - Successfully loaded asset from pack: {}", handle);
                return asset;
            }
        }

        OLO_CORE_ERROR("RuntimeAssetManager::LoadAssetFromPack - Failed to load asset from any pack: {}", handle);
        return nullptr;
    }

    bool RuntimeAssetManager::AssetExistsInPacks(AssetHandle handle) const
    {
        // Check if we have metadata for this asset
        TSharedLock<FSharedMutex> lock(m_PacksMutex);
        return m_AssetMetadata.find(handle) != m_AssetMetadata.end();
    }

    AssetType RuntimeAssetManager::GetAssetTypeFromPacks(AssetHandle handle) const
    {
        TSharedLock<FSharedMutex> lock(m_PacksMutex);
        auto it = m_AssetMetadata.find(handle);
        return (it != m_AssetMetadata.end()) ? it->second.Type : AssetType::None;
    }

    void RuntimeAssetManager::IndexAssetPackMetadata(const AssetPack& assetPack)
    {
        // Caller holds an exclusive lock on m_PacksMutex.
        //
        // Every metadata entry created here must be resolvable by LoadAssetFromPack so
        // that "has metadata" stays equivalent to "loadable from pack":
        //  - Non-scene assets resolve through AssetPack::GetAssetInfo (the AssetInfo
        //    table / lookup map), so index the top-level AssetInfos.
        //  - Scenes resolve through AssetPack::GetSceneInfo (the dedicated SceneInfo
        //    table whose PackedOffset points at the scene bytes). Index those too, and
        //    record the Scene type so LoadAssetFromPack dispatches them to the scene
        //    path rather than the AssetInfo path (whose offset is unset for scenes).
        for (const auto& info : assetPack.GetAllAssetInfos())
        {
            if (info.Handle == 0)
                continue;

            AssetMetadata metadata(info.Handle, info.Type);
            metadata.Status = AssetStatus::NotLoaded;
            m_AssetMetadata[info.Handle] = metadata;
        }

        for (const auto& sceneInfo : assetPack.GetAllSceneInfos())
        {
            if (sceneInfo.Handle == 0)
                continue;

            AssetMetadata metadata(sceneInfo.Handle, AssetType::Scene);
            metadata.Status = AssetStatus::NotLoaded;
            m_AssetMetadata[sceneInfo.Handle] = metadata;
        }
    }

} // namespace OloEngine
