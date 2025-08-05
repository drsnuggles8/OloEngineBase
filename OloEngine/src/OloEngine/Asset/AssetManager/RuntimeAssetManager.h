#pragma once

#include "AssetManagerBase.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSystem/RuntimeAssetSystem.h"
#include "OloEngine/Asset/AssetPack.h"

#include <shared_mutex>
#include <unordered_map>

namespace OloEngine
{
    /**
     * @brief Runtime asset manager optimized for shipping builds
     * 
     * The RuntimeAssetManager is designed for production/shipping builds where
     * assets are pre-packed and optimized. It loads from asset pack files rather
     * than individual files, providing:
     * 
     * - Fast asset loading from packed files
     * - Memory-efficient asset streaming
     * - No filesystem dependency for individual assets
     * - Optimized for performance over flexibility
     * 
     * Key differences from EditorAssetManager:
     * - No hot-reload support
     * - No individual file monitoring
     * - Asset pack-based loading
     * - Simplified dependency tracking
     */
    class RuntimeAssetManager : public AssetManagerBase
    {
    public:
        RuntimeAssetManager();
        virtual ~RuntimeAssetManager();

        // AssetManagerBase interface implementation
        virtual void Shutdown() override;

        virtual AssetType GetAssetType(AssetHandle assetHandle) override;
        virtual Ref<Asset> GetAsset(AssetHandle assetHandle) override;
        virtual AsyncAssetResult<Asset> GetAssetAsync(AssetHandle assetHandle) override;

        virtual void AddMemoryOnlyAsset(Ref<Asset> asset) override;
        virtual bool ReloadData(AssetHandle assetHandle) override;
        virtual void ReloadDataAsync(AssetHandle assetHandle) override;
        virtual bool EnsureCurrent(AssetHandle assetHandle) override;
        virtual bool EnsureAllLoadedCurrent() override;
        virtual bool IsAssetHandleValid(AssetHandle assetHandle) override;
        virtual Ref<Asset> GetMemoryAsset(AssetHandle handle) override;
        virtual bool IsAssetLoaded(AssetHandle handle) override;
        virtual bool IsAssetValid(AssetHandle handle) override;
        virtual bool IsAssetMissing(AssetHandle handle) override;
        virtual bool IsMemoryAsset(AssetHandle handle) override;
        virtual bool IsPhysicalAsset(AssetHandle handle) override;
        virtual void RemoveAsset(AssetHandle handle) override;

        // Dependency management (simplified for runtime)
        virtual void RegisterDependency(AssetHandle dependency, AssetHandle handle) override;
        virtual void DeregisterDependency(AssetHandle dependency, AssetHandle handle) override;
        virtual void DeregisterDependencies(AssetHandle handle) override;
        virtual std::unordered_set<AssetHandle> GetDependencies(AssetHandle handle) override;

        virtual void SyncWithAssetThread() override;

        virtual std::unordered_set<AssetHandle> GetAllAssetsWithType(AssetType type) override;
        virtual const std::unordered_map<AssetHandle, Ref<Asset>>& GetLoadedAssets() override 
        { 
            return m_LoadedAssets; 
        }

        // Runtime-specific methods

        /**
         * @brief Load an asset pack for runtime use
         * @param packPath Path to the asset pack file
         * @return True if the pack was successfully loaded
         */
        bool LoadAssetPack(const std::filesystem::path& packPath);

        /**
         * @brief Unload an asset pack
         * @param packPath Path to the asset pack file
         */
        void UnloadAssetPack(const std::filesystem::path& packPath);

        /**
         * @brief Get asset metadata from loaded packs
         * @param handle Asset handle
         * @return Asset metadata if found
         */
        AssetMetadata GetAssetMetadata(AssetHandle handle);

        /**
         * @brief Update dependencies when an asset changes
         * @param handle Handle of the changed asset
         */
        void UpdateDependencies(AssetHandle handle);

    private:
        /**
         * @brief Load an asset from the asset pack system
         * @param handle Asset handle to load
         * @return Loaded asset or nullptr if failed
         */
        Ref<Asset> LoadAssetFromPack(AssetHandle handle);

        /**
         * @brief Check if an asset exists in any loaded pack
         * @param handle Asset handle to check
         * @return True if asset exists in packs
         */
        bool AssetExistsInPacks(AssetHandle handle) const;

        /**
         * @brief Get asset type from pack metadata
         * @param handle Asset handle
         * @return Asset type or AssetType::None if not found
         */
        AssetType GetAssetTypeFromPacks(AssetHandle handle) const;

    private:
        // Loaded assets cache
        std::unordered_map<AssetHandle, Ref<Asset>> m_LoadedAssets;
        
        // Memory-only assets (no backing pack file)
        std::unordered_map<AssetHandle, Ref<Asset>> m_MemoryAssets;
        
        // Asset pack management
        std::unordered_map<std::filesystem::path, Ref<AssetPack>> m_LoadedPacks;
        
        // Asset metadata from packs
        std::unordered_map<AssetHandle, AssetMetadata> m_AssetMetadata;
        
        // Simplified dependency tracking for runtime
        std::unordered_map<AssetHandle, std::unordered_set<AssetHandle>> m_AssetDependencies;
        
        // Async asset loading system
        Ref<RuntimeAssetSystem> m_AssetThread;
        
        // Thread synchronization
        mutable std::shared_mutex m_AssetsMutex;
        mutable std::shared_mutex m_PacksMutex;
        mutable std::shared_mutex m_DependenciesMutex;
    };

}
