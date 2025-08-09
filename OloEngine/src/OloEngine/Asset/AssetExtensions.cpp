#include "AssetExtensions.h"

#include "OloEngine/Core/Log.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace OloEngine
{
    // Static member initialization
    std::unordered_map<std::string, AssetType> AssetExtensions::s_ExtensionMap;
    bool AssetExtensions::s_Initialized = false;

    AssetType AssetExtensions::GetAssetTypeFromExtension(const std::string& extension)
    {
        if (!s_Initialized)
            InitializeExtensionMap();

        std::string normalizedExt = NormalizeExtension(extension);
        
        auto it = s_ExtensionMap.find(normalizedExt);
        if (it != s_ExtensionMap.end())
            return it->second;

        // Check the static map for compatibility
        auto staticIt = s_AssetExtensionMap.find("." + normalizedExt);
        return (staticIt != s_AssetExtensionMap.end()) ? staticIt->second : AssetType::None;
    }

    AssetType AssetExtensions::GetAssetTypeFromPath(const std::string& filepath)
    {
        sizet dotPos = filepath.find_last_of('.');
        if (dotPos == std::string::npos)
            return AssetType::None;

        std::string extension = filepath.substr(dotPos);
        return GetAssetTypeFromExtension(extension);
    }

    bool AssetExtensions::IsExtensionSupported(const std::string& extension)
    {
        return GetAssetTypeFromExtension(extension) != AssetType::None;
    }

    std::vector<std::string> AssetExtensions::GetExtensionsForAssetType(AssetType type)
    {
        if (!s_Initialized)
            InitializeExtensionMap();

        std::unordered_set<std::string> extensionSet;

        for (const auto& [ext, assetType] : s_ExtensionMap)
        {
            if (assetType == type)
                extensionSet.insert("." + ext);
        }

        // Also check static map
        for (const auto& [ext, assetType] : s_AssetExtensionMap)
        {
            if (assetType == type)
                extensionSet.insert(ext);
        }

        return std::vector<std::string>(extensionSet.begin(), extensionSet.end());
    }

    std::vector<std::string> AssetExtensions::GetAllSupportedExtensions()
    {
        if (!s_Initialized)
            InitializeExtensionMap();

        std::unordered_set<std::string> extensionSet;

        for (const auto& [ext, type] : s_ExtensionMap)
        {
            extensionSet.insert("." + ext);
        }

        // Add extensions from static map
        for (const auto& [ext, type] : s_AssetExtensionMap)
        {
            extensionSet.insert(ext);
        }

        return std::vector<std::string>(extensionSet.begin(), extensionSet.end());
    }

    const std::unordered_map<std::string, AssetType>& AssetExtensions::GetExtensionMap()
    {
        if (!s_Initialized)
            InitializeExtensionMap();

        return s_ExtensionMap;
    }

    void AssetExtensions::InitializeExtensionMap()
    {
        if (s_Initialized)
            return;

        s_ExtensionMap.clear();

        // OloEngine types (normalized without dots)
        s_ExtensionMap["oloscene"] = AssetType::Scene;
        s_ExtensionMap["olomesh"] = AssetType::Mesh;
        s_ExtensionMap["olosmesh"] = AssetType::StaticMesh;
        s_ExtensionMap["olomaterial"] = AssetType::Material;
        s_ExtensionMap["oloanimation"] = AssetType::AnimationClip;
        s_ExtensionMap["oloanimgraph"] = AssetType::AnimationGraph;
        s_ExtensionMap["oloprefab"] = AssetType::Prefab;
        s_ExtensionMap["olosoundc"] = AssetType::SoundConfig;
        s_ExtensionMap["olomc"] = AssetType::MeshCollider;
        s_ExtensionMap["olosoundgraph"] = AssetType::SoundGraphSound;

        // Script files
        s_ExtensionMap["cs"] = AssetType::ScriptFile;

        // Mesh/animation source files
        s_ExtensionMap["fbx"] = AssetType::MeshSource;
        s_ExtensionMap["gltf"] = AssetType::MeshSource;
        s_ExtensionMap["glb"] = AssetType::MeshSource;
        s_ExtensionMap["obj"] = AssetType::MeshSource;
        s_ExtensionMap["dae"] = AssetType::MeshSource;
        s_ExtensionMap["vrm"] = AssetType::MeshSource;

        // Textures
        s_ExtensionMap["png"] = AssetType::Texture2D;
        s_ExtensionMap["jpg"] = AssetType::Texture2D;
        s_ExtensionMap["jpeg"] = AssetType::Texture2D;
        s_ExtensionMap["tga"] = AssetType::Texture2D;
        s_ExtensionMap["bmp"] = AssetType::Texture2D;
        s_ExtensionMap["hdr"] = AssetType::EnvMap;
        s_ExtensionMap["exr"] = AssetType::EnvMap;

        // Audio
        s_ExtensionMap["wav"] = AssetType::Audio;
        s_ExtensionMap["ogg"] = AssetType::Audio;
        s_ExtensionMap["mp3"] = AssetType::Audio;
        s_ExtensionMap["flac"] = AssetType::Audio;

        // Fonts
        s_ExtensionMap["ttf"] = AssetType::Font;
        s_ExtensionMap["ttc"] = AssetType::Font;
        s_ExtensionMap["otf"] = AssetType::Font;

        s_Initialized = true;
        OLO_CORE_INFO("AssetExtensions initialized with {} supported extensions", s_ExtensionMap.size());
    }

    std::string AssetExtensions::NormalizeExtension(const std::string& extension)
    {
        std::string normalized = extension;

        // Remove leading dot if present
        if (!normalized.empty() && normalized[0] == '.')
            normalized = normalized.substr(1);

        // Convert to lowercase
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                      [](unsigned char c) { return std::tolower(c); });

        return normalized;
    }

}
