#include "AssetExtensions.h"

#include "OloEngine/Core/Log.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <mutex>

namespace OloEngine
{
    AssetType AssetExtensions::GetAssetTypeFromExtension(std::string_view extension)
    {
        std::call_once(s_InitFlag, InitializeExtensionMap);

        std::string normalizedExt = NormalizeExtension(std::string(extension));

        auto it = s_ExtensionMap.find(normalizedExt);
        return (it != s_ExtensionMap.end()) ? it->second : AssetType::None;
    }

    AssetType AssetExtensions::GetAssetTypeFromPath(const std::string& filepath)
    {
        std::string::size_type dotPos = filepath.find_last_of('.');
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
        std::call_once(s_InitFlag, InitializeExtensionMap);

        std::vector<std::string> extensions;
        extensions.reserve(s_ExtensionMap.size()); // Reserve capacity to optimize memory allocation

        for (const auto& [ext, assetType] : s_ExtensionMap)
        {
            if (assetType == type)
                extensions.push_back("." + ext);
        }

        std::sort(extensions.begin(), extensions.end());
        return extensions;
    }

    std::vector<std::string> AssetExtensions::GetAllSupportedExtensions()
    {
        std::call_once(s_InitFlag, InitializeExtensionMap);

        std::vector<std::string> extensions;
        extensions.reserve(s_ExtensionMap.size()); // Reserve capacity to optimize memory allocation

        for (const auto& [ext, type] : s_ExtensionMap)
        {
            extensions.push_back("." + ext);
        }

        std::sort(extensions.begin(), extensions.end());
        return extensions;
    }

    const std::unordered_map<std::string, AssetType>& AssetExtensions::GetExtensionMap()
    {
        std::call_once(s_InitFlag, InitializeExtensionMap);

        return s_ExtensionMap;
    }

    void AssetExtensions::InitializeExtensionMap()
    {
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
                       [](unsigned char c)
                       { return std::tolower(c); });

        return normalized;
    }

} // namespace OloEngine
