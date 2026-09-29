#pragma once

#include "AssetManagerBase.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetSystem/RuntimeAssetSystem.h"
#include "OloEngine/Asset/AssetPack.h"
#include "OloEngine/Threading/SharedMutex.h"

#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    /**
     * @brief The asset-streaming side of the runtime byte accounting (issue #1365).
     *
     * Loads covers the async queue: pending requests with their estimated bytes,
     * and what cancellation did. Resident covers pack-backed assets in the loaded
     * cache (memory-only assets are not streamed and are not counted): each one
     * contributes its measured size, else its pack estimate, else counts as unknown.
     * Placeholders standing in for an asset that failed to load count as unknown:
     * they are shared, so their bytes are not this asset's.
     */
    struct FRuntimeAssetStreamingReport
    {
        FRuntimeAssetLoadStats Loads;
        FAssetByteTotal Resident;
    };

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
        // The async system delegates its worker-thread pack reads back to the manager's
        // LoadAssetFromPack (the single source of truth for loaded packs).
        friend class RuntimeAssetSystem;

      public:
        /**
         * @param autoLoadDefaultPack When true (default), the constructor loads
         *        "Assets/AssetPack.olopack" relative to the working directory if it
         *        exists. Pass false when the caller manages packs explicitly or needs
         *        a deterministic, empty starting state (e.g. tests).
         */
        explicit RuntimeAssetManager(bool autoLoadDefaultPack = true);
        virtual ~RuntimeAssetManager();

        // AssetManagerBase interface implementation
        void Shutdown() noexcept override;

        AssetType GetAssetType(AssetHandle assetHandle) const noexcept override;
        Ref<Asset> GetAsset(AssetHandle assetHandle) override;
        AsyncAssetResult<Asset> GetAssetAsync(AssetHandle assetHandle) override;
        AssetMetadata GetAssetMetadata(AssetHandle handle) const noexcept override;

        void AddMemoryOnlyAsset(Ref<Asset> asset) override;
        [[nodiscard]] bool ReloadData(AssetHandle assetHandle) override;
        void ReloadDataAsync(AssetHandle assetHandle) override;
        [[nodiscard]] bool EnsureCurrent(AssetHandle assetHandle) override;
        [[nodiscard]] bool EnsureAllLoadedCurrent() override;
        [[nodiscard]] bool IsAssetHandleValid(AssetHandle assetHandle) const noexcept override;
        Ref<Asset> GetMemoryAsset(AssetHandle handle) const override;
        [[nodiscard]] bool IsAssetLoaded(AssetHandle handle) const noexcept override;
        [[nodiscard]] bool IsAssetValid(AssetHandle handle) const noexcept override;
        [[nodiscard]] bool IsAssetMissing(AssetHandle handle) const noexcept override;
        [[nodiscard]] bool IsMemoryAsset(AssetHandle handle) const noexcept override;
        [[nodiscard]] bool IsPhysicalAsset(AssetHandle handle) const noexcept override;
        void RemoveAsset(AssetHandle handle) override;

        // Dependency management (simplified for runtime)
        void RegisterDependency(AssetHandle handle, AssetHandle dependency) override;
        void DeregisterDependency(AssetHandle handle, AssetHandle dependency) override;
        void DeregisterDependencies(AssetHandle handle) override;
        std::unordered_set<AssetHandle> GetDependencies(AssetHandle handle) const override;
        std::unordered_map<AssetHandle, std::unordered_set<AssetHandle>> GetAllDependencies() const override;

        void SyncWithAssetThread() noexcept override;

        std::unordered_set<AssetHandle> GetAllAssetsWithType(AssetType type) const override;
        std::unordered_map<AssetHandle, Ref<Asset>> GetLoadedAssets() const override;
        void ForEachLoadedAsset(const std::function<bool(AssetHandle, const Ref<Asset>&)>& callback) const override;

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

        // ── Async load cancellation and byte accounting (issue #1365) ─────────

        /**
         * @brief Cancel a pending async load. See RuntimeAssetSystem's cancellation
         *        contract.
         *
         * Ordered against SyncWithAssetThread(): both hold m_AssetsMutex across the
         * system call, so a result is either integrated before this returns
         * NotPending, or it is never integrated. A caller that wants the asset gone
         * either way calls RemoveAsset() after a NotPending.
         */
        EAssetLoadCancelResult CancelAssetLoad(AssetHandle handle);

        /**
         * @brief The byte size of an asset, available before its load completes.
         *
         * Resident: measured (Actual) when the asset type measures itself, else the
         * pack estimate. Pending: the estimate the request was queued with. Neither:
         * the pack estimate, so a caller can budget before it requests. Unknown when
         * no figure exists — never zero.
         */
        [[nodiscard]] FAssetByteSize GetAssetByteSize(AssetHandle handle) const;

        /**
         * @brief The pack's packed size of an asset, as an Estimate of its resident
         *        size; Unknown when no loaded pack records a non-zero size for it.
         */
        [[nodiscard]] FAssetByteSize EstimateAssetByteSizeFromPacks(AssetHandle handle) const;

        [[nodiscard]] FRuntimeAssetStreamingReport GetStreamingReport() const;

        /// Test seam forwarded to the async system; a no-op without OLO_ASYNC_ASSETS.
        void SetAsyncLoadTestHooks(FRuntimeAssetLoadTestHooks hooks);

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

        // The size of a cached asset. Caller holds m_AssetsMutex (either mode).
        [[nodiscard]] FAssetByteSize ResidentByteSizeOf(AssetHandle handle, const Ref<Asset>& asset) const;

        /**
         * @brief Index an asset pack's contents into the metadata table
         *
         * Populates m_AssetMetadata with one entry per asset in the pack so the
         * metadata-keyed query and load paths (GetAsset, IsAssetValid,
         * GetAssetType, GetAllAssetsWithType, ...) can resolve the pack's assets.
         * Without this step a loaded pack is invisible to every one of those paths.
         *
         * @param assetPack Loaded asset pack to index
         * @note The caller must hold an exclusive lock on m_PacksMutex.
         */
        void IndexAssetPackMetadata(const AssetPack& assetPack);

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

        // Handles for which a valid-but-unresolvable warning has already been logged,
        // so a corrupt asset substituted with a placeholder warns once, not per call.
        // Guarded by m_AssetsMutex (only touched while it is held exclusively).
        std::unordered_set<AssetHandle> m_WarnedUnresolvableHandles;

        // Async asset loading system
        Ref<RuntimeAssetSystem> m_AssetThread;

        // Thread synchronization
        mutable FSharedMutex m_AssetsMutex;
        mutable FSharedMutex m_PacksMutex;
        mutable FSharedMutex m_DependenciesMutex;
    };

} // namespace OloEngine
