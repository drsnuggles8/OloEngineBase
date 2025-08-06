#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/AssetManager/AssetManagerBase.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Project/Project.h"

#include <functional>
#include <unordered_map>
#include <vector>

// Asynchronous asset loading can be disabled by setting this to 0
// If you do this, then assets will not be automatically reloaded if/when they are changed by some external tool,
// and you will have to manually reload them via content browser panel.
#define OLO_ASYNC_ASSETS 1

namespace OloEngine
{
    /**
     * @brief Static facade for asset management operations
     * 
     * Provides a unified static API that abstracts the dual-manager system
     * (EditorAssetManager vs RuntimeAssetManager). All operations are forwarded
     * to the appropriate manager based on the current project configuration.
     * 
     * This class serves as the primary interface for all asset operations
     * throughout the engine, providing type-safe template methods and
     * convenient static access patterns.
     */
    class AssetManager
    {
    public:
        /**
         * @brief Check if an asset handle could potentially be valid
         * @param assetHandle Handle to validate
         * @return True if the handle could be valid (says nothing about the asset itself)
         */
        static bool IsAssetHandleValid(AssetHandle assetHandle) 
        { 
            return GetActiveManager()->IsAssetHandleValid(assetHandle); 
        }

        /**
         * @brief Check if an asset is valid and can be used
         * @param assetHandle Handle of the asset to check
         * @return True if the asset is valid (will attempt to load if not already loaded)
         * 
         * An asset is invalid if any of the following are true:
         * - The asset handle is invalid
         * - The file referred to by asset metadata is missing
         * - The asset could not be loaded from file
         */
        static bool IsAssetValid(AssetHandle assetHandle) 
        { 
            return GetActiveManager()->IsAssetValid(assetHandle); 
        }

        /**
         * @brief Check if an asset file is missing
         * @param assetHandle Handle of the asset to check
         * @return True if the asset file is missing
         * 
         * Note: This checks for file existence but doesn't attempt to load the asset.
         * Memory-only assets cannot be missing.
         */
        static bool IsAssetMissing(AssetHandle assetHandle) 
        { 
            return GetActiveManager()->IsAssetMissing(assetHandle); 
        }

        /**
         * @brief Check if an asset exists only in memory
         * @param handle Handle of the asset
         * @return True if asset has no backing file
         */
        static bool IsMemoryAsset(AssetHandle handle) 
        { 
            return GetActiveManager()->IsMemoryAsset(handle); 
        }

        /**
         * @brief Check if an asset has a backing file
         * @param handle Handle of the asset
         * @return True if asset has a physical file
         */
        static bool IsPhysicalAsset(AssetHandle handle) 
        { 
            return GetActiveManager()->IsPhysicalAsset(handle); 
        }

        /**
         * @brief Reload asset data from file synchronously
         * @param assetHandle Handle of the asset to reload
         * @return True if reload was successful
         */
        static bool ReloadData(AssetHandle assetHandle) 
        { 
            return GetActiveManager()->ReloadData(assetHandle); 
        }

        /**
         * @brief Ensure a specific asset is current (reload if modified)
         * @param assetHandle Handle of the asset to check
         * @return True if asset is current or was successfully updated
         */
        static bool EnsureCurrent(AssetHandle assetHandle) 
        { 
            return GetActiveManager()->EnsureCurrent(assetHandle); 
        }

        /**
         * @brief Ensure all loaded assets are current
         * @return True if all assets are current or were successfully updated
         */
        static bool EnsureAllLoadedCurrent() 
        { 
            return GetActiveManager()->EnsureAllLoadedCurrent(); 
        }

        /**
         * @brief Get the type of an asset
         * @param assetHandle Handle of the asset
         * @return AssetType of the asset
         */
        static AssetType GetAssetType(AssetHandle assetHandle) 
        { 
            return GetActiveManager()->GetAssetType(assetHandle); 
        }

        /**
         * @brief Synchronize with the asset loading thread
         * 
         * Ensures any pending async operations are completed or processed
         */
        static void SyncWithAssetThread() 
        { 
            return GetActiveManager()->SyncWithAssetThread(); 
        }

        /**
         * @brief Get a placeholder asset for a specific type
         * @param type Asset type to get placeholder for
         * @return Reference to placeholder asset
         */
        static Ref<Asset> GetPlaceholderAsset(AssetType type);

        /**
         * @brief Get an asset synchronously with type safety
         * @tparam T Asset type to retrieve
         * @param assetHandle Handle of the asset
         * @return Typed reference to the asset, or nullptr if not found/invalid
         */
        template<typename T>
        static Ref<T> GetAsset(AssetHandle assetHandle)
        {
            Ref<Asset> asset = GetActiveManager()->GetAsset(assetHandle);
            return asset.As<T>();
        }

        /**
         * @brief Get an asset asynchronously with type safety
         * @tparam T Asset type to retrieve
         * @param assetHandle Handle of the asset
         * @return AsyncAssetResult containing the typed asset and ready state
         */
        template<typename T>
        static AsyncAssetResult<T> GetAssetAsync(AssetHandle assetHandle)
        {
#if OLO_ASYNC_ASSETS
            AsyncAssetResult<Asset> result = GetActiveManager()->GetAssetAsync(assetHandle);
            return AsyncAssetResult<T>(result);
#else
            return { GetAsset<T>(assetHandle), true };
#endif
        }

        /**
         * @brief Get all assets of a specific type
         * @tparam T Asset type to search for
         * @return Set of asset handles of the specified type
         */
        template<typename T>
        static std::unordered_set<AssetHandle> GetAllAssetsWithType()
        {
            return GetActiveManager()->GetAllAssetsWithType(T::GetStaticType());
        }

        /**
         * @brief Get all currently loaded assets
         * @return Map of asset handles to asset references
         */
        static const std::unordered_map<AssetHandle, Ref<Asset>>& GetLoadedAssets() 
        { 
            return GetActiveManager()->GetLoadedAssets(); 
        }

        /**
         * @brief Add a memory-only asset to the manager
         * @tparam TAsset Asset type (must derive from Asset)
         * @param asset Asset to add
         * @return Handle of the added asset
         * 
         * Note: The memory-only asset must be fully initialized before calling this function.
         * Assets are not thread-safe themselves but can be accessed from multiple threads.
         * Thread safety depends on assets being immutable once added to the asset manager.
         */
        template<typename TAsset>
        static AssetHandle AddMemoryOnlyAsset(Ref<TAsset> asset)
        {
            static_assert(std::is_base_of<Asset, TAsset>::value, 
                         "AddMemoryOnlyAsset only works for types derived from Asset");
            
            if (!asset->Handle)
            {
                asset->Handle = AssetHandle(); // Generate new handle
            }
            
            GetActiveManager()->AddMemoryOnlyAsset(asset);
            return asset->Handle;
        }

        /**
         * @brief Get memory-only asset if it exists
         * @param handle Handle of the memory asset
         * @return Asset reference if exists in memory only, nullptr otherwise
         */
        static Ref<Asset> GetMemoryAsset(AssetHandle handle) 
        { 
            return GetActiveManager()->GetMemoryAsset(handle); 
        }

        /**
         * @brief Register that an asset depends on another asset
         * @param dependency Handle of the dependency asset
         * @param handle Handle of the dependent asset
         * 
         * Example: A material (handle) depends on a texture (dependency)
         */
        static void RegisterDependency(AssetHandle dependency, AssetHandle handle) 
        { 
            return GetActiveManager()->RegisterDependency(dependency, handle); 
        }

        /**
         * @brief Remove a specific dependency relationship
         * @param dependency Handle of the dependency asset
         * @param handle Handle of the dependent asset
         */
        static void DeregisterDependency(AssetHandle dependency, AssetHandle handle) 
        { 
            return GetActiveManager()->DeregisterDependency(dependency, handle); 
        }

        /**
         * @brief Remove all dependencies of an asset
         * @param handle Handle of the asset to clear dependencies for
         */
        static void DeregisterDependencies(AssetHandle handle) 
        { 
            return GetActiveManager()->DeregisterDependencies(handle); 
        }

        /**
         * @brief Remove an asset from the manager
         * @param handle Handle of the asset to remove
         */
        static void RemoveAsset(AssetHandle handle)
        {
            GetActiveManager()->RemoveAsset(handle);
        }

    private:
        static Ref<AssetManagerBase> GetActiveManager()
        {
            auto manager = Project::GetAssetManager();
            OLO_CORE_ASSERT(manager, "Asset manager not initialized");
            return manager;
        }
    };

}
