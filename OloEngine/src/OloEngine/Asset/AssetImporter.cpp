#include "OloEnginePCH.h"
#include "AssetImporter.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"

#include <memory>

namespace OloEngine
{
    std::unordered_map<AssetType, Scope<AssetSerializer>> AssetImporter::s_Serializers;

    void AssetImporter::Init()
    {
        s_Serializers.clear();
        s_Serializers.reserve(17); // Reserve capacity for 17 serializers to avoid rehashing
        s_Serializers[AssetType::Prefab] = CreateScope<PrefabSerializer>();
        s_Serializers[AssetType::Texture2D] = CreateScope<TextureSerializer>();
        s_Serializers[AssetType::TextureCube] = CreateScope<TextureSerializer>();
        s_Serializers[AssetType::Mesh] = CreateScope<MeshSerializer>();
        s_Serializers[AssetType::StaticMesh] = CreateScope<StaticMeshSerializer>();
        s_Serializers[AssetType::MeshSource] = CreateScope<MeshSourceSerializer>();
        s_Serializers[AssetType::Material] = CreateScope<MaterialAssetSerializer>();
        s_Serializers[AssetType::EnvMap] = CreateScope<EnvironmentSerializer>();
        s_Serializers[AssetType::Audio] = CreateScope<AudioFileSourceSerializer>();
        s_Serializers[AssetType::SoundConfig] = CreateScope<SoundConfigSerializer>();
        s_Serializers[AssetType::Scene] = CreateScope<SceneAssetSerializer>();
        s_Serializers[AssetType::Font] = CreateScope<FontSerializer>();
        s_Serializers[AssetType::MeshCollider] = CreateScope<MeshColliderSerializer>();
        s_Serializers[AssetType::SoundGraphSound] = CreateScope<SoundGraphSerializer>();
        s_Serializers[AssetType::AnimationClip] = CreateScope<AnimationAssetSerializer>();
        s_Serializers[AssetType::AnimationGraph] = CreateScope<AnimationGraphAssetSerializer>();
        s_Serializers[AssetType::ScriptFile] = CreateScope<ScriptFileSerializer>();
    }

    void AssetImporter::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset)
    {
        auto it = s_Serializers.find(metadata.Type);
        if (it == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
            return;
        }

        it->second->Serialize(metadata, asset);
    }

    void AssetImporter::Serialize(const Ref<Asset>& asset)
    {
        // Get metadata from asset manager
        auto assetManagerBase = Project::GetAssetManager();
        if (!assetManagerBase)
        {
            OLO_CORE_WARN("Asset manager not available");
            return;
        }
        
        AssetMetadata metadata = assetManagerBase->GetAssetMetadata(asset->m_Handle);
        Serialize(metadata, asset);
    }

    bool AssetImporter::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset)
    {
        auto it = s_Serializers.find(metadata.Type);
        if (it == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
            return false;
        }

        return it->second->TryLoadData(metadata, asset);
    }

    void AssetImporter::RegisterDependencies(const AssetMetadata& metadata)
    {
        auto it = s_Serializers.find(metadata.Type);
        if (it == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
            return;
        }

        it->second->RegisterDependencies(metadata);
    }

    bool AssetImporter::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo)
    {
        outInfo.Size = 0;

        if (!AssetManager::IsAssetHandleValid(handle))
            return false;

        auto asset = AssetManager::GetAsset<Asset>(handle);
        if (!asset)
        {
            OLO_CORE_WARN("Failed to get asset with handle {0}", handle);
            return false;
        }

        AssetType type = asset->GetAssetType();
        auto it = s_Serializers.find(type);
        if (it == s_Serializers.end())
        {
            OLO_CORE_WARN("There's currently no serializer for assets of type: {}", AssetUtils::AssetTypeToString(type));
            return false;
        }

        return it->second->SerializeToAssetPack(handle, stream, outInfo);
    }

    Ref<Asset> AssetImporter::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo)
    {
        auto it = s_Serializers.find(assetInfo.Type);
        if (it == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(assetInfo.Type));
            return nullptr;
        }

        return it->second->DeserializeFromAssetPack(stream, assetInfo);
    }

    Ref<Scene> AssetImporter::DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& assetInfo)
    {
        auto it = s_Serializers.find(AssetType::Scene);
        if (it == s_Serializers.end())
        {
            OLO_CORE_WARN("Scene serializer not available");
            return nullptr;
        }

        auto sceneSerializer = it->second.get();
        return sceneSerializer->DeserializeSceneFromAssetPack(stream, assetInfo);
    }

} // namespace OloEngine
