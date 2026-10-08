#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/AssetPack.h"

#include <filesystem>
#include <atomic>
#include <unordered_map>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include <optional>

namespace OloEngine
{
    struct AssetPackBuildRecord
    {
        AssetHandle Handle = 0;
        AssetType Type = AssetType::None;
        FString Path;
        u64 Offset = 0;
        u64 Size = 0;
        std::optional<u32> TextureFormat;
        bool TextureSRGB = false;
        u32 TextureWidth = 0;
        u32 TextureHeight = 0;
    };
    template<>
    struct TIsTriviallyRelocatable<AssetPackBuildRecord>
    {
        static constexpr bool Value =
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::Handle)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::Type)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::Path)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::Offset)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::Size)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::TextureFormat)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::TextureSRGB)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::TextureWidth)> &&
            TIsTriviallyRelocatable_V<decltype(AssetPackBuildRecord::TextureHeight)>;
    };
    /**
     * @brief Utility class for building asset packs from project assets
     *
     * The AssetPackBuilder scans the project's asset registry and serializes
     * all assets into a single pack file for runtime distribution.
     * Inspired by Hazel's AssetPack::CreateFromActiveProject implementation.
     */
    class AssetPackBuilder final
    {
      public:
        using AssetRecord = AssetPackBuildRecord;
        /**
         * @brief Build result information
         */
        struct [[nodiscard]] BuildResult
        {
            bool m_Success = false;
            std::string m_ErrorMessage;
            sizet m_AssetCount = 0;
            sizet m_SceneCount = 0;
            std::filesystem::path m_OutputPath;
            // Registered assets that did not load and are therefore NOT in the pack.
            sizet m_FailedAssetCount = 0;
            TArray<AssetRecord> m_Records;
        };

        /**
         * @brief Build settings for asset pack creation
         */
        struct BuildSettings
        {
            std::filesystem::path m_OutputPath = "Assets/AssetPack.olopack";
            bool m_CompressAssets = true;
            bool m_IncludeScriptModule = true;
            bool m_ValidateAssets = true;
            // Copy `.ololocale` files from `assets/localization/` into the
            // output dir alongside the pack. Localization files aren't
            // registered as asset-manager assets (they're configuration
            // that LocalizationManager owns directly), so they need a
            // side-channel into the shipped game's working directory.
            bool m_IncludeLocalizationFiles = true;
        };

      public:
        // Prevent instantiation - this is a static utility class
        AssetPackBuilder() = delete;

        /**
         * @brief Create asset pack from active project
         * @param settings Build settings for the pack
         * @param progress Atomic progress tracker (0.0 to 1.0)
         * @param cancelToken Optional cancellation token for cooperative cancellation
         * @return Build result with success/failure info
         */
        static BuildResult BuildFromActiveProject(const BuildSettings& settings, std::atomic<f32>& progress, const std::atomic<bool>* cancelToken = nullptr);

        /**
         * @brief Create asset pack from specific asset registry
         * @param assetRegistry Asset registry to build from
         * @param settings Build settings for the pack
         * @param progress Atomic progress tracker (0.0 to 1.0)
         * @param cancelToken Optional cancellation token for cooperative cancellation
         * @return Build result with success/failure info
         */
        static BuildResult BuildFromRegistry(const AssetRegistry& assetRegistry, const BuildSettings& settings, std::atomic<f32>& progress, const std::atomic<bool>* cancelToken = nullptr);

        /**
         * @brief The colour space each texture's material slots want (issue #1462)
         *
         * Walks every material among `assets` (MeshSource imported materials and Material
         * assets) and maps each referenced texture handle to true for a base-colour or
         * emissive slot (sRGB), false for metallic-roughness / normal / AO (linear data),
         * the same rule the importer uses (Model::LoadMaterialTextures). A texture used by
         * both kinds of slot is reported with a warning naming both and cooked sRGB: a
         * colour map sampled linearly washes the whole material out, and a texture shared
         * between a colour and a data slot is an authoring mistake that a ".oloimport"
         * ColorSpace settles explicitly.
         */
        [[nodiscard]] static std::unordered_map<AssetHandle, bool> CollectTextureColorSpaceIntents(
            const std::unordered_map<AssetHandle, Ref<Asset>>& assets);

      private:
        /**
         * @brief One stage's share of the caller's progress: a stage writes 0..1 of its
         *        own work and the caller's atomic receives Base + Scale * value. It
         *        replaced a forwarding task that ran forever when no task worker was
         *        free to take it, because it then ran inline on this thread (#1533).
         */
        struct ProgressRange
        {
            std::atomic<f32>& Out;
            f32 Base = 0.0f;
            f32 Scale = 1.0f;

            ProgressRange& operator=(f32 value)
            {
                Out.store(Base + (Scale * value), std::memory_order_relaxed);
                return *this;
            }
        };

        /**
         * @brief Build asset pack implementation
         * @param assetManager Asset manager to use
         * @param settings Build settings
         * @param progress The build's share of the caller's progress
         * @param cancelToken Optional cancellation token for cooperative cancellation
         * @return Build result
         */
        static BuildResult BuildImpl(Ref<AssetManagerBase> assetManager, const BuildSettings& settings, ProgressRange progress, const std::atomic<bool>* cancelToken = nullptr);

        /**
         * @brief Serialize all assets from asset manager to pack
         * @param assetManager Asset manager to read from
         * @param assetPackFile Pack file to write to
         * @param scriptModuleSize Bytes of the script module the pack will carry (0 for none):
         *        the data is placed after it, so it must be the module the writer writes
         * @param progress Progress tracker
         * @param cancelToken Optional cancellation token for cooperative cancellation
         * @return Success status
         */
        [[nodiscard]] static bool SerializeAllAssets(Ref<AssetManagerBase> assetManager, AssetPackFile& assetPackFile, u64 scriptModuleSize, BuildResult& result, ProgressRange progress, const std::atomic<bool>* cancelToken = nullptr);

        /**
         * @brief Validate that all assets can be serialized
         * @param assetManager Asset manager to validate
         * @return Validation result
         */
        [[nodiscard]] static bool ValidateAssets(Ref<AssetManagerBase> assetManager);

        /**
         * @brief Get script module binary if available
         * @return Script module binary data
         */
        static Buffer GetScriptModuleBinary();
    };
} // namespace OloEngine
