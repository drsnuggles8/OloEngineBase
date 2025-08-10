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
        virtual void Shutdown() noexcept override;

        virtual AssetType GetAssetType(AssetHandle assetHandle) const noexcept override;
        virtual Ref<Asset> GetAsset(AssetHandle assetHandle) override;
        virtual AsyncAssetResult<Asset> GetAssetAsync(AssetHandle assetHandle) override;
        virtual AssetMetadata GetAssetMetadata(AssetHandle handle) const noexcept override;

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

        // Dependency management (simplified for runtime)
        virtual void RegisterDependency(AssetHandle handle, AssetHandle dependency) override;
        virtual void DeregisterDependency(AssetHandle handle, AssetHandle dependency) override;
        virtual void DeregisterDependencies(AssetHandle handle) override;
        virtual std::unordered_set<AssetHandle> GetDependencies(AssetHandle handle) const override;

        virtual void SyncWithAssetThread() noexcept override;

        virtual std::unordered_set<AssetHandle> GetAllAssetsWithType(AssetType type) const override;
        virtual std::unordered_map<AssetHandle, Ref<Asset>> GetLoadedAssets() const override;
        virtual void ForEachLoadedAsset(const std::function<bool(AssetHandle, const Ref<Asset>&)>& callback) const override;

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
         * @brief Get asset metadata from loaded packs (runtime-specific version)
         * @param handle Asset handle
         * @return Asset metadata if found
         */
        AssetMetadata GetAssetMetadataFromPacks(AssetHandle handle);

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
