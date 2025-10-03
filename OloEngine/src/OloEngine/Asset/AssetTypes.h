#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Assert.h"
#include <string>

namespace OloEngine
{
    enum class AssetType : u16
    {
        None = 0,
        Scene = 1,
        Prefab = 2,
        Mesh = 3,
        StaticMesh = 4,
        MeshSource = 5,
        Material = 6,
        Texture2D = 7,
        TextureCube = 8,
        EnvMap = 9,
        Audio = 10,
        SoundConfig = 11,
        SpatializationConfig = 12,
        Font = 13,
        Script = 14,
        ScriptFile = 15,
        Shader = 16,
        MeshCollider = 17,
        SoundGraphSound = 18,
        AnimationClip = 19,
        AnimationGraph = 20,
        Model = 21,
        Environment = 22,
        SoundGraph = 23
    };

    enum class AssetFlag : u16
    {
        None = 0,
        Missing = OloBit16(0),
        Invalid = OloBit16(1)
    };

    // Bitwise operators for AssetFlag to enable flag operations
    inline AssetFlag operator|(AssetFlag lhs, AssetFlag rhs)
    {
        return static_cast<AssetFlag>(static_cast<u16>(lhs) | static_cast<u16>(rhs));
    }

    inline AssetFlag operator&(AssetFlag lhs, AssetFlag rhs)
    {
        return static_cast<AssetFlag>(static_cast<u16>(lhs) & static_cast<u16>(rhs));
    }

    inline AssetFlag operator^(AssetFlag lhs, AssetFlag rhs)
    {
        return static_cast<AssetFlag>(static_cast<u16>(lhs) ^ static_cast<u16>(rhs));
    }

    inline AssetFlag operator~(AssetFlag flag)
    {
        return static_cast<AssetFlag>(~static_cast<u16>(flag));
    }

    inline AssetFlag& operator|=(AssetFlag& lhs, AssetFlag rhs)
    {
        lhs = lhs | rhs;
        return lhs;
    }

    inline AssetFlag& operator&=(AssetFlag& lhs, AssetFlag rhs)
    {
        lhs = lhs & rhs;
        return lhs;
    }

    inline AssetFlag& operator^=(AssetFlag& lhs, AssetFlag rhs)
    {
        lhs = lhs ^ rhs;
        return lhs;
    }

    /**
     * @brief Utility functions for asset type management
     */
    namespace AssetUtils
    {
        /**
         * @brief Convert AssetType enum to string representation
         * @param type The asset type to convert
         * @return String representation of the asset type
         */
        inline const char* AssetTypeToString(AssetType assetType)
        {
            switch (assetType)
            {
                case AssetType::None:                return "None";
                case AssetType::Scene:               return "Scene";
                case AssetType::Prefab:              return "Prefab";
                case AssetType::Mesh:                return "Mesh";
                case AssetType::StaticMesh:          return "StaticMesh";
                case AssetType::MeshSource:          return "MeshSource";
                case AssetType::Material:            return "Material";
                case AssetType::Texture2D:           return "Texture2D";
                case AssetType::TextureCube:         return "TextureCube";
                case AssetType::EnvMap:              return "EnvMap";
                case AssetType::Audio:               return "Audio";
                case AssetType::SoundConfig:         return "SoundConfig";
                case AssetType::SpatializationConfig: return "SpatializationConfig";
                case AssetType::Font:                return "Font";
                case AssetType::Script:              return "Script";
                case AssetType::ScriptFile:          return "ScriptFile";
                case AssetType::Shader:              return "Shader";
                case AssetType::MeshCollider:        return "MeshCollider";
                case AssetType::SoundGraphSound:     return "SoundGraphSound";
                case AssetType::AnimationClip:       return "AnimationClip";
                case AssetType::AnimationGraph:      return "AnimationGraph";
                case AssetType::Model:               return "Model";
                case AssetType::Environment:         return "Environment";
                case AssetType::SoundGraph:          return "SoundGraph";
            }
            
            OLO_CORE_ASSERT(false, "Unknown Asset Type");
            return "None";
        }
        
        /**
         * @brief Convert string representation to AssetType enum
         * @param assetType String representation of the asset type
         * @return AssetType enum value, or AssetType::None if not found
         */
        inline AssetType AssetTypeFromString(const std::string& assetType)
        {
            if (assetType == "None")                return AssetType::None;
            if (assetType == "Scene")               return AssetType::Scene;
            if (assetType == "Prefab")              return AssetType::Prefab;
            if (assetType == "Mesh")                return AssetType::Mesh;
            if (assetType == "StaticMesh")          return AssetType::StaticMesh;
            if (assetType == "MeshAsset")           return AssetType::MeshSource; // DEPRECATED
            if (assetType == "MeshSource")          return AssetType::MeshSource;
            if (assetType == "Material")            return AssetType::Material;
            if (assetType == "Texture2D")           return AssetType::Texture2D;
            if (assetType == "TextureCube")         return AssetType::TextureCube;
            if (assetType == "EnvMap")              return AssetType::EnvMap;
            if (assetType == "Audio")               return AssetType::Audio;
            if (assetType == "SoundConfig")         return AssetType::SoundConfig;
            if (assetType == "SpatializationConfig") return AssetType::SpatializationConfig;
            if (assetType == "Font")                return AssetType::Font;
            if (assetType == "Script")              return AssetType::Script;
            if (assetType == "ScriptFile")          return AssetType::ScriptFile;
            if (assetType == "Shader")              return AssetType::Shader;
            if (assetType == "MeshCollider")        return AssetType::MeshCollider;
            if (assetType == "SoundGraphSound")     return AssetType::SoundGraphSound;
            if (assetType == "AnimationClip")       return AssetType::AnimationClip;
            //if (assetType == "AnimationController") return AssetType::AnimationController; // OBSOLETE. You need to re-import animated asset
            if (assetType == "AnimationGraph")      return AssetType::AnimationGraph;
            if (assetType == "Model")               return AssetType::Model;
            if (assetType == "Environment")         return AssetType::Environment;
            if (assetType == "SoundGraph")          return AssetType::SoundGraph;

            return AssetType::None;
        }

        /**
         * @brief Check if an asset type is a valid runtime type
         * @param type The asset type to check
         * @return True if the type is valid for runtime use
         */
        inline bool IsAssetTypeValid(AssetType type)
        {
            return type != AssetType::None;
        }
    }

}
