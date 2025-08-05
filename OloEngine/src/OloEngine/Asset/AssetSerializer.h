#pragma once

#include "AssetMetadata.h"
#include "MeshColliderAsset.h"

#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Serialization/AssetPackFile.h"

namespace OloEngine
{
    // Forward declarations
    class MaterialAsset;
    class MeshColliderAsset;
    class PhysicsMaterial;
    class Prefab;
    class Scene;
    struct SoundConfig;
    class AnimationAsset;
    class AnimationGraphAsset;

    struct AssetSerializationInfo
    {
        uint64_t Offset = 0;
        uint64_t Size = 0;
    };

    class AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const = 0;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const = 0;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const {}

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const = 0;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const = 0;
    };

    class TextureSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override {}
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class FontSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override {}
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class MaterialAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    private:
        std::string SerializeToYAML(Ref<MaterialAsset> materialAsset) const;
        std::string GetYAML(const AssetMetadata& metadata) const;
        void RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<MaterialAsset>& targetMaterialAsset, AssetHandle handle) const;
    };

    class EnvironmentSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override {}
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class AudioFileSourceSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class SoundConfigSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    private:
        std::string SerializeToYAML(Ref<SoundConfig> soundConfig) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<SoundConfig> targetSoundConfig) const;
    };

    class PrefabSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class SceneAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
        Ref<Scene> DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& sceneInfo) const;
    };
        
    class MeshColliderSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    private:
        std::string SerializeToYAML(Ref<MeshColliderAsset> meshCollider) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<MeshColliderAsset> targetMeshCollider) const;
    };

    class ScriptFileSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    // Missing serializers that Hazel has
    class MeshSourceSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override {}
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class MeshSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class StaticMeshSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class AnimationAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class AnimationGraphAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

    class SoundGraphGraphSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const;
    };

} // namespace OloEngine
