#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetTypes.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <mutex>

namespace OloEngine
{
    /**
     * @brief Static mapping of file extensions to asset types
     *
     * This class provides functionality to automatically detect asset types
     * based on file extensions. It supports common formats as well as
     * OloEngine-specific formats.
     */
    class AssetExtensions
    {
      public:
        /**
         * @brief Get asset type from file extension
         * @param extension File extension (with or without leading dot)
         * @return AssetType corresponding to the extension, or AssetType::None if not found
         */
        [[nodiscard]] static AssetType GetAssetTypeFromExtension(std::string_view extension);
        /**
         * @brief Get asset type from file path
         * @param filepath Full file path
         * @return AssetType corresponding to the file extension, or AssetType::None if not found
         */
        static AssetType GetAssetTypeFromPath(const std::string& filepath);

        /**
         * @brief Check if a file extension is supported
         * @param extension File extension (with or without leading dot)
         * @return True if the extension is supported
         */
        static bool IsExtensionSupported(const std::string& extension);

        /**
         * @brief Get all supported extensions for a specific asset type
         * @param type Asset type
         * @return Vector of supported extensions for the asset type
         */
        static std::vector<std::string> GetExtensionsForAssetType(AssetType type);

        /**
         * @brief Get all supported extensions
         * @return Vector of all supported file extensions
         */
        static std::vector<std::string> GetAllSupportedExtensions();

        /**
         * @brief Get the complete extension map (for direct access if needed)
         * @return Reference to the internal extension map
         */
        static const std::unordered_map<std::string, AssetType>& GetExtensionMap();

      private:
        /**
         * @brief Initialize the extension to asset type mapping
         */
        static void InitializeExtensionMap();

        /**
         * @brief Normalize extension by removing leading dot and converting to lowercase
         * @param extension Raw extension string
         * @return Normalized extension string
         */
        static std::string NormalizeExtension(const std::string& extension);

        // Static mapping from file extensions to asset types
        inline static std::unordered_map<std::string, AssetType> s_ExtensionMap;
        inline static std::once_flag s_InitFlag;
    };

    /**
     * @brief File extension constants for OloEngine-specific formats
     */
    namespace OloExtensions
    {
        constexpr const char* Scene = ".oloscene";
        constexpr const char* Mesh = ".olomesh";
        constexpr const char* StaticMesh = ".olosmesh";
        constexpr const char* Material = ".olomaterial";
        constexpr const char* Animation = ".oloanimation";
        constexpr const char* AnimationGraph = ".oloanimgraph";
        constexpr const char* SoundConfig = ".olosoundc";
        constexpr const char* SoundGraph = ".olosoundgraph";
        constexpr const char* Prefab = ".oloprefab";
        constexpr const char* Script = ".oloscript";
        constexpr const char* MeshCollider = ".olomc";
        constexpr const char* ParticleSystem = ".oloparticle";
    } // namespace OloExtensions

} // namespace OloEngine
