#include "OloEnginePCH.h"
#include "AssetImporter.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace OloEngine
{
    // Global flag to track if we're in static destruction
    static std::atomic<bool> g_IsInStaticDestruction{ false };

    // This object will be destroyed during static destruction and set the flag
    struct StaticDestructionSentinel
    {
        ~StaticDestructionSentinel()
        {
            g_IsInStaticDestruction.store(true, std::memory_order_release);
        }
    };

    // This static object will be destroyed early in the static destruction chain
    static StaticDestructionSentinel g_StaticSentinel;

    // Use function-local static to ensure proper construction/destruction order
    static std::unordered_map<AssetType, Scope<AssetSerializer>>& GetSerializers()
    {
        static std::unordered_map<AssetType, Scope<AssetSerializer>> s_Serializers;
        return s_Serializers;
    }

    // Use function-local static for mutex as well
    static std::mutex& GetSerializersMutex()
    {
        static std::mutex s_SerializersMutex;
        return s_SerializersMutex;
    }

    static std::once_flag s_InitFlag;
    static std::atomic<bool> s_IsShuttingDown{ false };

    void AssetImporter::Init()
    {
        std::call_once(s_InitFlag, []()
                       {
            auto& serializers = GetSerializers();
            serializers.clear();
            serializers.reserve(17); // Reserve capacity for 17 serializers to avoid rehashing
            serializers[AssetType::Prefab] = CreateScope<PrefabSerializer>();
            serializers[AssetType::Texture2D] = CreateScope<TextureSerializer>();
            serializers[AssetType::TextureCube] = CreateScope<TextureSerializer>();
            serializers[AssetType::Mesh] = CreateScope<MeshSerializer>();
            serializers[AssetType::StaticMesh] = CreateScope<StaticMeshSerializer>();
            serializers[AssetType::MeshSource] = CreateScope<MeshSourceSerializer>();
            serializers[AssetType::Material] = CreateScope<MaterialAssetSerializer>();
            serializers[AssetType::EnvMap] = CreateScope<EnvironmentSerializer>();
            serializers[AssetType::Audio] = CreateScope<AudioFileSourceSerializer>();
            // serializers[AssetType::SoundConfig] = CreateScope<SoundConfigSerializer>(); // Disabled - SoundConfig not implemented
            serializers[AssetType::Scene] = CreateScope<SceneAssetSerializer>();
            serializers[AssetType::Font] = CreateScope<FontSerializer>();
            serializers[AssetType::MeshCollider] = CreateScope<MeshColliderSerializer>();
            serializers[AssetType::SoundGraphSound] = CreateScope<SoundGraphSerializer>();
            serializers[AssetType::AnimationClip] = CreateScope<AnimationAssetSerializer>();
            serializers[AssetType::AnimationGraph] = CreateScope<AnimationGraphAssetSerializer>();
            serializers[AssetType::ScriptFile] = CreateScope<ScriptFileSerializer>(); });
    }

    void AssetImporter::Shutdown()
    {
        // Set shutdown flag to prevent re-entry during static destruction
        auto wasShuttingDown = s_IsShuttingDown.exchange(true, std::memory_order_acq_rel);
        if (wasShuttingDown)
        {
            // Already shutting down, avoid double shutdown
            return;
        }

        // If we're in static destruction, absolutely do not attempt any cleanup
        // The OS will handle memory cleanup automatically
        if (g_IsInStaticDestruction.load(std::memory_order_acquire))
        {
            return;
        }

        // Additional safety check: try to detect if we're being called during exit
        // by attempting to access the function-local statics safely
        try
        {
            // If accessing these throws or behaves oddly, we're likely in static destruction
            auto& mutex = GetSerializersMutex();
            auto& serializers = GetSerializers();

            // Try to lock with a very short timeout - if this fails, we might be in trouble
            if (!mutex.try_lock())
            {
                // Can't acquire lock quickly, might be in static destruction
                return;
            }

            // Use RAII to ensure unlock
            std::unique_lock<std::mutex> lock(mutex, std::adopt_lock);

            // Final check: if we're in static destruction by now, just return
            if (g_IsInStaticDestruction.load(std::memory_order_acquire))
            {
                return;
            }

            // Only clear if we're absolutely sure we're not in static destruction
            serializers.clear();
        }
        catch (...)
        {
            // Any exception during shutdown means we should not proceed
            // This is likely due to static destruction issues
        }
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
            std::scoped_lock lock(GetSerializersMutex());
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
        std::scoped_lock lock(GetSerializersMutex());
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
        std::scoped_lock lock(GetSerializersMutex());
        auto& serializers = GetSerializers();
        auto it = serializers.find(type);
        if (it == serializers.end())
        {
            return false;
        }

        return it->second->SupportsAsyncLoading();
    }

    bool AssetImporter::TryLoadRawData(const AssetMetadata& metadata, RawAssetData& outRawData)
    {
        std::scoped_lock lock(GetSerializersMutex());
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
        std::scoped_lock lock(GetSerializersMutex());
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
        std::scoped_lock lock(GetSerializersMutex());
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
            std::scoped_lock lock(GetSerializersMutex());
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
        std::scoped_lock lock(GetSerializersMutex());
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
        std::scoped_lock lock(GetSerializersMutex());
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
