#include "OloEnginePCH.h"
#include "AssetImporter.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"

namespace OloEngine
{
    std::unordered_map<AssetType, Scope<AssetSerializer>> AssetImporter::s_Serializers;

    void AssetImporter::Init()
    {
        s_Serializers.clear();
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
        s_Serializers[AssetType::SoundGraphSound] = CreateScope<SoundGraphGraphSerializer>();
        s_Serializers[AssetType::AnimationClip] = CreateScope<AnimationAssetSerializer>();
        s_Serializers[AssetType::AnimationGraph] = CreateScope<AnimationGraphAssetSerializer>();
        s_Serializers[AssetType::ScriptFile] = CreateScope<ScriptFileSerializer>();
    }

    void AssetImporter::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset)
    {
        if (s_Serializers.find(metadata.Type) == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type");
            return;
        }

        s_Serializers[metadata.Type]->Serialize(metadata, asset);
    }

    void AssetImporter::Serialize(const Ref<Asset>& asset)
    {
        // Get metadata from editor asset manager
        auto assetManagerBase = Project::GetAssetManager();
        auto editorAssetManager = std::dynamic_pointer_cast<EditorAssetManager>(assetManagerBase);
        if (!editorAssetManager)
        {
            OLO_CORE_WARN("Editor asset manager not available");
            return;
        }
        
        const AssetMetadata& metadata = editorAssetManager->GetMetadata(asset->Handle);
        Serialize(metadata, asset);
    }

    bool AssetImporter::TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset)
    {
        if (s_Serializers.find(metadata.Type) == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type");
            return false;
        }

        return s_Serializers[metadata.Type]->TryLoadData(metadata, asset);
    }

    void AssetImporter::RegisterDependencies(const AssetMetadata& metadata)
    {
        if (s_Serializers.find(metadata.Type) == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type");
            return;
        }

        s_Serializers[metadata.Type]->RegisterDependencies(metadata);
    }

    bool AssetImporter::SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo)
    {
        outInfo.Size = 0;

        if (!AssetManager::IsAssetHandleValid(handle))
            return false;

        if (auto asset = AssetManager::GetAsset<Asset>(handle); !asset)
        {
            OLO_CORE_WARN("Failed to get asset with handle {0}", handle);
            return false;
        }

        AssetType type = AssetManager::GetAssetType(handle);
        if (s_Serializers.find(type) == s_Serializers.end())
        {
            OLO_CORE_WARN("There's currently no serializer for assets of type");
            return false;
        }

        return s_Serializers[type]->SerializeToAssetPack(handle, stream, outInfo);
    }

    Ref<Asset> AssetImporter::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo)
    {
        if (s_Serializers.find(assetInfo.Type) == s_Serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type");
            return nullptr;
        }

        return s_Serializers[assetInfo.Type]->DeserializeFromAssetPack(stream, assetInfo);
    }

    Ref<Scene> AssetImporter::DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& assetInfo)
    {
        auto sceneSerializer = dynamic_cast<SceneAssetSerializer*>(s_Serializers[AssetType::Scene].get());
        if (!sceneSerializer)
        {
            OLO_CORE_WARN("Scene serializer not available");
            return nullptr;
        }

        return sceneSerializer->DeserializeSceneFromAssetPack(stream, assetInfo);
    }

} // namespace OloEngine
