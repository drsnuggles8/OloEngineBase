#include "OloEnginePCH.h"
#include "AssetImporter.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"

#include <memory>
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

namespace OloEngine
{
    // Use function-local static to ensure proper construction/destruction order
    static std::unordered_map<AssetType, Scope<AssetSerializer>>& GetSerializers()
    {
        static std::unordered_map<AssetType, Scope<AssetSerializer>> s_Serializers;
        return s_Serializers;
    }

    // Use function-local static for mutex as well
    static FMutex& GetSerializersMutex()
    {
        static FMutex s_SerializersMutex;
        return s_SerializersMutex;
    }

    void AssetImporter::Init()
    {
        // The serializer registry is process-lifetime and shared by every asset manager
        // (it holds stateless serializers — no GPU/file handles). Populate it once, lazily,
        // on the first manager's Init(); Shutdown() intentionally does not clear it, so a
        // second manager (editor<->runtime transitions, tests) keeps working and one
        // manager's teardown can't wipe the registry out from under another.
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        if (!serializers.empty())
            return;

        serializers.reserve(32); // Reserve ahead of the registered serializer count (28) to avoid rehashing
        serializers[AssetType::Prefab] = CreateScope<PrefabSerializer>();
        serializers[AssetType::Texture2D] = CreateScope<TextureSerializer>();
        serializers[AssetType::TextureCube] = CreateScope<TextureSerializer>();
        serializers[AssetType::Mesh] = CreateScope<MeshSerializer>();
        serializers[AssetType::StaticMesh] = CreateScope<StaticMeshSerializer>();
        serializers[AssetType::MeshSource] = CreateScope<MeshSourceSerializer>();
        serializers[AssetType::Material] = CreateScope<MaterialAssetSerializer>();
        serializers[AssetType::EnvMap] = CreateScope<EnvironmentSerializer>();
        serializers[AssetType::Audio] = CreateScope<AudioFileSourceSerializer>();
        serializers[AssetType::SoundConfig] = CreateScope<SoundConfigSerializer>();
        serializers[AssetType::Scene] = CreateScope<SceneAssetSerializer>();
        serializers[AssetType::Font] = CreateScope<FontSerializer>();
        serializers[AssetType::MeshCollider] = CreateScope<MeshColliderSerializer>();
        serializers[AssetType::SoundGraph] = CreateScope<SoundGraphSerializer>();
        serializers[AssetType::AnimationClip] = CreateScope<AnimationAssetSerializer>();
        serializers[AssetType::AnimationGraph] = CreateScope<AnimationGraphAssetSerializer>();
        serializers[AssetType::ScriptFile] = CreateScope<ScriptFileSerializer>();
        serializers[AssetType::ParticleSystem] = CreateScope<ParticleSystemAssetSerializer>();
        serializers[AssetType::LightProbeVolume] = CreateScope<LightProbeVolumeSerializer>();
        serializers[AssetType::DialogueTree] = CreateScope<DialogueTreeSerializer>();
        serializers[AssetType::ShaderGraph] = CreateScope<ShaderGraphSerializer>();
        serializers[AssetType::BehaviorTree] = CreateScope<BehaviorTreeSerializer>();
        serializers[AssetType::StateMachine] = CreateScope<StateMachineSerializer>();
        serializers[AssetType::InstancePlacement] = CreateScope<InstancePlacementSerializer>();
        serializers[AssetType::CinematicSequence] = CreateScope<CinematicSequenceAssetSerializer>();
        serializers[AssetType::FluidSettings] = CreateScope<FluidSettingsAssetSerializer>();
        serializers[AssetType::ExperienceCurve] = CreateScope<ExperienceCurveSerializer>();
        serializers[AssetType::SkillTreeDatabase] = CreateScope<SkillTreeDatabaseSerializer>();
        serializers[AssetType::CharacterClassDatabase] = CreateScope<CharacterClassDatabaseSerializer>();
    }

    void AssetImporter::Shutdown()
    {
        // Intentional no-op. The serializer registry is process-lifetime: serializers are
        // stateless, so there is nothing to release per manager, and clearing here would
        // wipe the shared registry for any other asset manager still alive (and break a
        // subsequently constructed one). The function-local static registry is destroyed
        // naturally at program exit. Kept for the symmetric Init()/Shutdown() API and so
        // managers don't need to change their teardown.
    }

    void AssetImporter::Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset)
    {
        if (!asset)
        {
            OLO_CORE_ERROR("AssetImporter::Serialize - Asset reference is null for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
            return;
        }

        AssetType actualType = asset->GetAssetType();
        if (metadata.Type != actualType)
        {
            OLO_CORE_WARN("AssetImporter::Serialize - Asset type mismatch: metadata type {} does not match actual asset type {}",
                          AssetUtils::AssetTypeToString(metadata.Type), AssetUtils::AssetTypeToString(actualType));
            return;
        }

        {
            TUniqueLock<FMutex> lock(GetSerializersMutex());
            auto& serializers = GetSerializers();
            auto it = serializers.find(metadata.Type);
            if (it == serializers.end())
            {
                OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
                return;
            }

            it->second->Serialize(metadata, asset);
        }
    }

    void AssetImporter::Serialize(const Ref<Asset>& asset)
    {
        if (!asset)
        {
            OLO_CORE_ERROR("AssetImporter::Serialize - Asset reference is null");
            return;
        }

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
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(metadata.Type);
        if (it == serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
            return false;
        }

        return it->second->TryLoadData(metadata, asset);
    }

    bool AssetImporter::SupportsAsyncLoading(AssetType type)
    {
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(type);
        if (it == serializers.end())
        {
            return false;
        }

        return it->second->SupportsAsyncLoading();
    }

    bool AssetImporter::CanDeserializeFromAssetPackOffThread(AssetType type)
    {
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(type);
        if (it == serializers.end())
        {
            return false;
        }

        return it->second->CanDeserializeFromAssetPackOffThread();
    }

    bool AssetImporter::TryLoadRawData(const AssetMetadata& metadata, RawAssetData& outRawData)
    {
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(metadata.Type);
        if (it == serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
            return false;
        }

        if (!it->second->SupportsAsyncLoading())
        {
            OLO_CORE_WARN("Asset type {} does not support async loading", AssetUtils::AssetTypeToString(metadata.Type));
            return false;
        }

        return it->second->TryLoadRawData(metadata, outRawData);
    }

    bool AssetImporter::FinalizeFromRawData(const AssetMetadata& metadata, RawAssetData& rawData, Ref<Asset>& outAsset)
    {
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(metadata.Type);
        if (it == serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(metadata.Type));
            return false;
        }

        if (!it->second->FinalizeFromRawData(rawData, outAsset))
        {
            OLO_CORE_ERROR("Failed to finalize asset from raw data: {}", metadata.FilePath.string());
            return false;
        }

        if (outAsset)
        {
            outAsset->m_Handle = metadata.Handle;
        }

        return true;
    }

    void AssetImporter::RegisterDependencies(const AssetMetadata& metadata)
    {
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(metadata.Type);
        if (it == serializers.end())
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
        {
            TUniqueLock<FMutex> lock(GetSerializersMutex());
            auto& serializers = GetSerializers();
            auto it = serializers.find(type);
            if (it == serializers.end())
            {
                OLO_CORE_WARN("There's currently no serializer for assets of type: {}", AssetUtils::AssetTypeToString(type));
                return false;
            }

            return it->second->SerializeToAssetPack(handle, stream, outInfo);
        }
    }

    Ref<Asset> AssetImporter::DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo)
    {
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(assetInfo.Type);
        if (it == serializers.end())
        {
            OLO_CORE_WARN("No serializer available for asset type: {}", AssetUtils::AssetTypeToString(assetInfo.Type));
            return nullptr;
        }

        return it->second->DeserializeFromAssetPack(stream, assetInfo);
    }

    Ref<Scene> AssetImporter::DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& assetInfo)
    {
        TUniqueLock<FMutex> lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(AssetType::Scene);
        if (it == serializers.end())
        {
            OLO_CORE_WARN("Scene serializer not available");
            return nullptr;
        }

        auto sceneSerializer = it->second.get();
        return sceneSerializer->DeserializeSceneFromAssetPack(stream, assetInfo);
    }

} // namespace OloEngine
