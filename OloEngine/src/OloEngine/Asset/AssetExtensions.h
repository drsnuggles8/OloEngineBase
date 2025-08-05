#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetTypes.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>

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
        static AssetType GetAssetTypeFromExtension(const std::string& extension);

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
        static std::unordered_map<std::string, AssetType> s_ExtensionMap;
        static bool s_Initialized;
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
    }

    /**
     * @brief Static extension map similar to Hazel's approach for direct access
     * 
     * This provides the same interface as Hazel while maintaining the class-based
     * approach for additional functionality.
     */
    inline static std::unordered_map<std::string, AssetType> s_AssetExtensionMap =
    {
        // OloEngine types
        { ".oloscene", AssetType::Scene },
        { ".olomesh", AssetType::Mesh },
        { ".olosmesh", AssetType::StaticMesh },
        { ".olomaterial", AssetType::Material },
        { ".oloanimation", AssetType::AnimationClip },
        { ".oloanimgraph", AssetType::AnimationGraph },
        { ".oloprefab", AssetType::Prefab },
        { ".olosoundc", AssetType::SoundConfig },

        { ".cs", AssetType::ScriptFile },

        // mesh/animation source
        { ".fbx", AssetType::MeshSource },
        { ".gltf", AssetType::MeshSource },
        { ".glb", AssetType::MeshSource },
        { ".obj", AssetType::MeshSource },
        { ".dae", AssetType::MeshSource },
        { ".vrm", AssetType::MeshSource },

        // Textures
        { ".png", AssetType::Texture2D },
        { ".jpg", AssetType::Texture2D },
        { ".jpeg", AssetType::Texture2D },
        { ".tga", AssetType::Texture2D },
        { ".bmp", AssetType::Texture2D },
        { ".hdr", AssetType::EnvMap },
        { ".exr", AssetType::EnvMap },

        // Audio
        { ".wav", AssetType::Audio },
        { ".ogg", AssetType::Audio },
        { ".mp3", AssetType::Audio },
        { ".flac", AssetType::Audio },

        // Fonts
        { ".ttf", AssetType::Font },
        { ".ttc", AssetType::Font },
        { ".otf", AssetType::Font },
        
        // Mesh Collider
        { ".olomc", AssetType::MeshCollider },

        // Graphs
        { ".olosoundgraph", AssetType::SoundGraphSound }
    };

}
