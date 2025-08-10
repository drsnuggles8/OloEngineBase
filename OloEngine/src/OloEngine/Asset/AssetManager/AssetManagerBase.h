#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/AssetMetadata.h"

#include <unordered_set>
#include <unordered_map>

namespace OloEngine
{
    /**
     * @brief Abstract base class for asset management implementations
     * 
     * Defines the interface that both EditorAssetManager and RuntimeAssetManager
     * must implement. This provides a unified API for asset operations while
     * allowing different implementations for development and shipping builds.
     * 
     * The asset manager is responsible for:
     * - Loading and unloading assets
     * - Managing asset dependencies
     * - Tracking asset state and validity
     * - Providing synchronous and asynchronous asset access
     * - Memory-only asset management
     */
    class AssetManagerBase : public RefCounted
    {
    public:
        AssetManagerBase() = default;
        virtual ~AssetManagerBase() noexcept = default;

        /**
         * @brief Shutdown the asset manager and cleanup resources
         */
        virtual void Shutdown() noexcept = 0;

        /**
         * @brief Get the type of an asset by its handle
         * @param assetHandle Handle of the asset
         * @return AssetType of the asset, or AssetType::None if invalid
         */
        virtual AssetType GetAssetType(AssetHandle assetHandle) const noexcept = 0;

        /**
         * @brief Get an asset synchronously by handle
         * @param assetHandle Handle of the asset to retrieve
         * @return Reference to the asset, or nullptr if not found/invalid
         */
        virtual Ref<Asset> GetAsset(AssetHandle assetHandle) = 0;

        /**
         * @brief Get an asset asynchronously by handle
         * @param assetHandle Handle of the asset to retrieve
         * @return AsyncAssetResult containing the asset and ready state
         */
        virtual AsyncAssetResult<Asset> GetAssetAsync(AssetHandle assetHandle) = 0;

        /**
         * @brief Get asset metadata by handle
         * @param handle Handle of the asset
         * @return AssetMetadata for the asset, or empty metadata if not found
         * 
         * Note: Runtime asset managers may return limited metadata compared to editor managers
         */
        virtual AssetMetadata GetAssetMetadata(AssetHandle handle) const noexcept = 0;

        /**
         * @brief Add a memory-only asset (no backing file)
         * @param asset Asset to add to memory management
         */
        virtual void AddMemoryOnlyAsset(Ref<Asset> asset) = 0;

        /**
         * @brief Reload asset data from file synchronously
         * @param assetHandle Handle of the asset to reload
         * @return True if reload was successful
         */
        [[nodiscard]] virtual bool ReloadData(AssetHandle assetHandle) = 0;

        /**
         * @brief Reload asset data from file asynchronously
         * @param assetHandle Handle of the asset to reload
         */
        virtual void ReloadDataAsync(AssetHandle assetHandle) = 0;

        /**
         * @brief Ensure a specific asset is current (reload if modified)
         * @param assetHandle Handle of the asset to check
         * @return True if asset is current or was successfully updated
         */
        [[nodiscard]] virtual bool EnsureCurrent(AssetHandle assetHandle) = 0;

        /**
         * @brief Ensure all loaded assets are current
         * @return True if all assets are current or were successfully updated
         */
        [[nodiscard]] virtual bool EnsureAllLoadedCurrent() = 0;

        /**
         * @brief Check if an asset handle is potentially valid
         * @param assetHandle Handle to validate
         * @return True if the handle could be valid (says nothing about the asset itself)
         */
        [[nodiscard]] virtual bool IsAssetHandleValid(AssetHandle assetHandle) const noexcept = 0;

        /**
         * @brief Get memory-only asset if it exists
         * @param handle Handle of the memory asset
         * @return Asset reference if exists in memory only, nullptr otherwise
         */
        virtual Ref<Asset> GetMemoryAsset(AssetHandle handle) const = 0;

        /**
         * @brief Check if an asset has been loaded from file
         * @param handle Handle of the asset
         * @return True if asset has been loaded (could still be invalid)
         */
        [[nodiscard]] virtual bool IsAssetLoaded(AssetHandle handle) const noexcept = 0;

        /**
         * @brief Check if an asset is valid (loaded and not corrupted)
         * @param handle Handle of the asset
         * @return True if asset file was loaded and is valid
         */
        [[nodiscard]] virtual bool IsAssetValid(AssetHandle handle) const noexcept = 0;

        /**
         * @brief Check if an asset file is missing
         * @param handle Handle of the asset
         * @return True if asset file is missing (memory-only assets cannot be missing)
         */
        [[nodiscard]] virtual bool IsAssetMissing(AssetHandle handle) const noexcept = 0;

        /**
         * @brief Check if an asset exists only in memory
         * @param handle Handle of the asset
         * @return True if asset has no backing file
         */
        [[nodiscard]] virtual bool IsMemoryAsset(AssetHandle handle) const noexcept = 0;

        /**
         * @brief Check if an asset has a backing file
         * @param handle Handle of the asset
         * @return True if asset has a physical file
         */
        [[nodiscard]] virtual bool IsPhysicalAsset(AssetHandle handle) const noexcept = 0;

        /**
         * @brief Remove an asset from the manager
         * @param handle Handle of the asset to remove
         */
        virtual void RemoveAsset(AssetHandle handle) = 0;

        /**
         * @brief Register that an asset depends on another asset
         * @param dependency Handle of the dependency asset
         * @param handle Handle of the dependent asset
         * 
         * Example: A material (handle) depends on a texture (dependency)
         */
        virtual void RegisterDependency(AssetHandle dependency, AssetHandle handle) = 0;

        /**
         * @brief Remove a specific dependency relationship
         * @param dependency Handle of the dependency asset
         * @param handle Handle of the dependent asset
         */
        virtual void DeregisterDependency(AssetHandle dependency, AssetHandle handle) = 0;

        /**
         * @brief Remove all dependencies of an asset
         * @param handle Handle of the asset to clear dependencies for
         */
        virtual void DeregisterDependencies(AssetHandle handle) = 0;

        /**
         * @brief Get all dependencies of an asset
         * @param handle Handle of the asset
         * @return Set of handles that this asset depends on
         */
        virtual std::unordered_set<AssetHandle> GetDependencies(AssetHandle handle) const = 0;

        /**
         * @brief Synchronize with the asset loading thread
         * 
         * Ensures any pending async operations are completed or processed
         */
        virtual void SyncWithAssetThread() noexcept = 0;

        /**
         * @brief Get all assets of a specific type
         * @param type Asset type to search for
         * @return Set of asset handles of the specified type
         */
        virtual std::unordered_set<AssetHandle> GetAllAssetsWithType(AssetType type) const = 0;

        /**
         * @brief Get all currently loaded assets
         * @return Map of asset handles to asset references
         */
        virtual const std::unordered_map<AssetHandle, Ref<Asset>>& GetLoadedAssets() const noexcept = 0;
    };

}
