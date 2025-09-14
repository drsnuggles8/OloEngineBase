#pragma once

#include <string>
#include "AssetMetadata.h"
#include "MeshColliderAsset.h"

#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Serialization/AssetPackFile.h"

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    // Forward declarations
    class MaterialAsset;
    class PhysicsMaterial;
    class Prefab;
    class Scene;
    class ScriptFileAsset;
    // struct SoundConfig;  // TODO(olbu): Implement once soundconfig exists
    class AnimationAsset;
    class AnimationGraphAsset;

    struct AssetSerializationInfo
    {
        u64 Offset = 0;
        u64 Size = 0;
    };

    class AssetSerializer
    {
    public:
        virtual ~AssetSerializer() = default;
        
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const = 0;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const = 0;
        virtual void RegisterDependencies([[maybe_unused]] const AssetMetadata& metadata) const {}

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const = 0;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const = 0;
        
        // Virtual method for scene-specific deserialization, returns nullptr by default
        virtual Ref<Scene> DeserializeSceneFromAssetPack([[maybe_unused]] FileStreamReader& stream, [[maybe_unused]] const AssetPackFile::SceneInfo& sceneInfo) const { return nullptr; }
    };

    class TextureSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const override {}
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class FontSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const override {}
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class MaterialAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    private:
        std::string SerializeToYAML(Ref<MaterialAsset> materialAsset) const;
        std::string GetYAML(const AssetMetadata& metadata) const;
        void RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<MaterialAsset>& targetMaterialAsset, AssetHandle handle) const;
    };

    class EnvironmentSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const override {}
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class AudioFileSourceSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    
    private:
        bool GetWavFileInfo(const std::filesystem::path& filePath, double& duration, u32& samplingRate, u16& bitDepth, u16& numChannels) const;
    };

    // SoundConfig not implemented yet - commented out to avoid compilation errors
    /*
    class SoundConfigSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    private:
        std::string SerializeToYAML(Ref<SoundConfig> soundConfig) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<SoundConfig> targetSoundConfig) const;
    };
    */

    class PrefabSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    
    private:
        std::string SerializeToYAML(Ref<Prefab> prefab) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<Prefab> prefab) const;
    };

    class SceneAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        virtual Ref<Scene> DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& sceneInfo) const override;
        
        // String serialization methods for asset pack support
        std::string SerializeToString(const Ref<Scene>& scene) const;
        bool DeserializeFromString(const std::string& yamlString, Ref<Scene>& scene) const;
    };
        
    class MeshColliderSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    private:
        std::string SerializeToYAML(Ref<MeshColliderAsset> meshCollider) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<MeshColliderAsset> targetMeshCollider) const;
        void RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const;
    };

    class ScriptFileSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    private:
        std::string SerializeToYAML(Ref<ScriptFileAsset> scriptAsset) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<ScriptFileAsset> targetScriptAsset) const;
    };

    // Missing serializers that Hazel has
    class MeshSourceSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override { (void)metadata; (void)asset; }
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class MeshSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class StaticMeshSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class AnimationAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

    private:
        std::string SerializeToYAML(Ref<AnimationAsset> animationAsset) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<AnimationAsset>& animationAsset) const;
        bool DeserializeFromYAML(const YAML::Node& data, Ref<AnimationAsset>& animationAsset) const; // For pre-parsed YAML nodes
        void RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const;
    };

    class AnimationGraphAssetSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        virtual void RegisterDependencies(const AssetMetadata& metadata) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class SoundGraphSerializer : public AssetSerializer
    {
    public:
        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

} // namespace OloEngine
