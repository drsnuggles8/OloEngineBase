#include "RuntimeAssetManager.h"

#include "OloEngine/Asset/AssetImporter.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Timer.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    RuntimeAssetManager::RuntimeAssetManager()
    {
#if OLO_ASYNC_ASSETS
        m_AssetThread = Ref<RuntimeAssetSystem>::Create();
#endif

        AssetImporter::Init();
        OLO_CORE_INFO("RuntimeAssetManager initialized");
        
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
        std::scoped_lock lock(m_AssetsMutex, m_PacksMutex, m_DependenciesMutex);
        m_LoadedAssets.clear();
        m_MemoryAssets.clear();
        m_LoadedPacks.clear();
        m_AssetDependencies.clear();
        
        // Shutdown AssetImporter to release serializer resources
        AssetImporter::Shutdown();
    }

    AssetType RuntimeAssetManager::GetAssetType(AssetHandle assetHandle) const noexcept
    {
        // Check loaded assets and memory assets under a single shared lock
        {
            std::shared_lock lock(m_AssetsMutex);
            
            // Check loaded assets first
            auto it = m_LoadedAssets.find(assetHandle);
            if (it != m_LoadedAssets.end() && it->second)
            {
                return it->second->GetAssetType();
            }

            // Check memory assets
            auto memIt = m_MemoryAssets.find(assetHandle);
            if (memIt != m_MemoryAssets.end() && memIt->second)
            {
                return memIt->second->GetAssetType();
            }
        }

        // Check asset pack metadata
        return GetAssetTypeFromPacks(assetHandle);
    }

    AssetMetadata RuntimeAssetManager::GetAssetMetadata(AssetHandle handle) const noexcept
    {
        std::shared_lock lock(m_PacksMutex);
        
        // Return copy of metadata from the loaded packs storage
        auto it = m_AssetMetadata.find(handle);
        if (it != m_AssetMetadata.end())
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
            std::shared_lock lock(m_AssetsMutex);
            
            // Check loaded assets first
            auto it = m_LoadedAssets.find(assetHandle);
            if (it != m_LoadedAssets.end())
                return it->second;

            // Check memory assets
            auto memIt = m_MemoryAssets.find(assetHandle);
            if (memIt != m_MemoryAssets.end())
                return memIt->second;
        }

        // Asset not found, need to load it - acquire unique lock for double-checked locking
        {
            std::unique_lock lock(m_AssetsMutex);
            
            // Second check: verify asset wasn't loaded by another thread while we waited for the lock
            auto it = m_LoadedAssets.find(assetHandle);
            if (it != m_LoadedAssets.end())
                return it->second;

            // Check memory assets again
            auto memIt = m_MemoryAssets.find(assetHandle);
            if (memIt != m_MemoryAssets.end())
                return memIt->second;

            // Asset still not loaded, safe to load it now
            // Release the lock temporarily while loading from pack (expensive operation)
            lock.unlock();
            Ref<Asset> asset = LoadAssetFromPack(assetHandle);
            
            if (asset)
            {
                // Re-acquire lock to insert the loaded asset
                lock.lock();
                
                // Final check: ensure no other thread loaded it while we were loading
                auto finalIt = m_LoadedAssets.find(assetHandle);
                if (finalIt != m_LoadedAssets.end())
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
        // For runtime, we'll load synchronously for now
        // In a full implementation, this would queue the asset for background loading
        Ref<Asset> asset = GetAsset(assetHandle);
        return AsyncAssetResult<Asset>(asset, asset != nullptr);
    }

    void RuntimeAssetManager::AddMemoryOnlyAsset(Ref<Asset> asset)
    {
        if (!asset || asset->m_Handle == 0)
            return;

        std::unique_lock lock(m_AssetsMutex);
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
        std::shared_lock lock(m_AssetsMutex);
        auto it = m_MemoryAssets.find(handle);
        return (it != m_MemoryAssets.end()) ? it->second : nullptr;
    }

    bool RuntimeAssetManager::IsAssetLoaded(AssetHandle handle) const noexcept
    {
        std::shared_lock lock(m_AssetsMutex);
        return m_LoadedAssets.find(handle) != m_LoadedAssets.end();
    }

    bool RuntimeAssetManager::IsAssetValid(AssetHandle handle) const noexcept
    {
        // Check if it's loaded and valid
        {
            std::shared_lock lock(m_AssetsMutex);
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
        std::shared_lock lock(m_AssetsMutex);
        return m_MemoryAssets.find(handle) != m_MemoryAssets.end();
    }

    bool RuntimeAssetManager::IsPhysicalAsset(AssetHandle handle) const noexcept
    {
        return !IsMemoryAsset(handle) && AssetExistsInPacks(handle);
    }

    void RuntimeAssetManager::RemoveAsset(AssetHandle handle)
    {
        std::unique_lock lock(m_AssetsMutex);
        
        // Remove from loaded assets
        m_LoadedAssets.erase(handle);
        
        // Remove from memory assets
        m_MemoryAssets.erase(handle);
        
        // Remove dependencies
        DeregisterDependencies(handle);
    }

    void RuntimeAssetManager::RegisterDependency(AssetHandle handle, AssetHandle dependency)
    {
        std::unique_lock lock(m_DependenciesMutex);
        m_AssetDependencies[handle].insert(dependency);
    }

    void RuntimeAssetManager::DeregisterDependency(AssetHandle handle, AssetHandle dependency)
    {
        std::unique_lock lock(m_DependenciesMutex);
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
        std::unique_lock lock(m_DependenciesMutex);
        m_AssetDependencies.erase(handle);
    }

    std::unordered_set<AssetHandle> RuntimeAssetManager::GetDependencies(AssetHandle handle) const
    {
        std::shared_lock lock(m_DependenciesMutex);
        auto it = m_AssetDependencies.find(handle);
        return (it != m_AssetDependencies.end()) ? it->second : std::unordered_set<AssetHandle>{};
    }

    void RuntimeAssetManager::SyncWithAssetThread() noexcept
    {
        // Runtime manager doesn't use separate asset threads
        // All loading is done on the calling thread
    }

    std::unordered_set<AssetHandle> RuntimeAssetManager::GetAllAssetsWithType(AssetType type) const
    {
        std::unordered_set<AssetHandle> result;
        
        // Check loaded assets
        {
            std::shared_lock lock(m_AssetsMutex);
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
        for (const auto& [handle, metadata] : m_AssetMetadata)
        {
            if (metadata.Type == type)
                result.insert(handle);
        }
        
        return result;
    }

    std::unordered_map<AssetHandle, Ref<Asset>> RuntimeAssetManager::GetLoadedAssets() const
    {
        std::shared_lock lock(m_AssetsMutex);
        return m_LoadedAssets;
    }

    void RuntimeAssetManager::ForEachLoadedAsset(const std::function<bool(AssetHandle, const Ref<Asset>&)>& callback) const
    {
        std::shared_lock lock(m_AssetsMutex);
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
            std::shared_lock lock(m_PacksMutex);
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

        // Store the loaded pack
        {
            std::unique_lock lock(m_PacksMutex);
            m_LoadedPacks[packPath] = assetPack;
        }

        OLO_CORE_INFO("Loaded asset pack: {}", packPath.string());
        return true;
    }

    void RuntimeAssetManager::UnloadAssetPack(const std::filesystem::path& packPath)
    {
        std::unique_lock lock(m_PacksMutex);
        auto it = m_LoadedPacks.find(packPath);
        if (it != m_LoadedPacks.end())
        {
            it->second->Unload();
            m_LoadedPacks.erase(it);
            OLO_CORE_INFO("Unloaded asset pack: {}", packPath.string());
        }
    }

    AssetMetadata RuntimeAssetManager::GetAssetMetadataFromPacks(AssetHandle handle)
    {
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
        // Check if we have metadata for this asset
        auto metadataIt = m_AssetMetadata.find(handle);
        if (metadataIt == m_AssetMetadata.end())
        {
            OLO_CORE_ERROR("RuntimeAssetManager::LoadAssetFromPack - No metadata found for asset: {}", handle);
            return nullptr;
        }

        // Find which pack contains the asset
        std::shared_lock lock(m_PacksMutex);
        for (const auto& [packPath, assetPack] : m_LoadedPacks)
        {
            if (assetPack->IsAssetAvailable(handle))
            {
                // Get asset info from pack
                auto assetInfo = assetPack->GetAssetInfo(handle);
                if (!assetInfo.has_value())
                {
                    continue;
                }

                // Create file stream reader for the pack
                auto stream = assetPack->GetAssetStreamReader();
                if (!stream)
                {
                    continue;
                }

                // Use AssetImporter to deserialize from pack
                Ref<Asset> asset = AssetImporter::DeserializeFromAssetPack(*stream, assetInfo.value());
                if (asset)
                {
                    asset->m_Handle = handle;
                    OLO_CORE_TRACE("RuntimeAssetManager::LoadAssetFromPack - Successfully loaded asset from pack: {}", handle);
                    return asset;
                }
            }
        }
        
        OLO_CORE_ERROR("RuntimeAssetManager::LoadAssetFromPack - Failed to load asset from any pack: {}", handle);
        return nullptr;
    }

    bool RuntimeAssetManager::AssetExistsInPacks(AssetHandle handle) const
    {
        // Check if we have metadata for this asset
        return m_AssetMetadata.find(handle) != m_AssetMetadata.end();
    }

    AssetType RuntimeAssetManager::GetAssetTypeFromPacks(AssetHandle handle) const
    {
        auto it = m_AssetMetadata.find(handle);
        return (it != m_AssetMetadata.end()) ? it->second.Type : AssetType::None;
    }

}
