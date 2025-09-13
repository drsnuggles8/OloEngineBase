#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetRegistry.h"
#include "OloEngine/Asset/AssetPack.h"

#include <filesystem>
#include <atomic>

namespace OloEngine
{
    /**
     * @brief Utility class for building asset packs from project assets
     * 
     * The AssetPackBuilder scans the project's asset registry and serializes
     * all assets into a single pack file for runtime distribution.
     * Inspired by Hazel's AssetPack::CreateFromActiveProject implementation.
     */
    class AssetPackBuilder
    {
    public:
        /**
         * @brief Build result information
         */
        struct BuildResult
        {
            bool Success = false;
            std::string ErrorMessage;
            size_t AssetCount = 0;
            size_t SceneCount = 0;
            std::filesystem::path OutputPath;
        };

        /**
         * @brief Build settings for asset pack creation
         */
        struct BuildSettings
        {
            std::filesystem::path OutputPath = "Assets/AssetPack.olopack";
            bool CompressAssets = true;
            bool IncludeScriptModule = true;
            bool ValidateAssets = true;
        };

    public:
        /**
         * @brief Create asset pack from active project
         * @param settings Build settings for the pack
         * @param progress Atomic progress tracker (0.0 to 1.0)
         * @return Build result with success/failure info
         */
        static BuildResult BuildFromActiveProject(const BuildSettings& settings, std::atomic<float>& progress);

        /**
         * @brief Create asset pack from specific asset registry
         * @param assetRegistry Asset registry to build from
         * @param settings Build settings for the pack
         * @param progress Atomic progress tracker (0.0 to 1.0)
         * @return Build result with success/failure info
         */
        static BuildResult BuildFromRegistry(const AssetRegistry& assetRegistry, const BuildSettings& settings, std::atomic<float>& progress);

    private:
        /**
         * @brief Build asset pack implementation
         * @param assetManager Asset manager to use
         * @param settings Build settings
         * @param progress Progress tracker
         * @return Build result
         */
        static BuildResult BuildImpl(Ref<AssetManagerBase> assetManager, const BuildSettings& settings, std::atomic<float>& progress);

        /**
         * @brief Serialize all assets from asset manager to pack
         * @param assetManager Asset manager to read from
         * @param assetPackFile Pack file to write to
         * @param progress Progress tracker
         * @return Success status
         */
        static bool SerializeAllAssets(Ref<AssetManagerBase> assetManager, AssetPackFile& assetPackFile, std::atomic<float>& progress);

        /**
         * @brief Validate that all assets can be serialized
         * @param assetManager Asset manager to validate
         * @return Validation result
         */
        static bool ValidateAssets(Ref<AssetManagerBase> assetManager);

        /**
         * @brief Get script module binary if available
         * @return Script module binary data
         */
        static Buffer GetScriptModuleBinary();
    };
}