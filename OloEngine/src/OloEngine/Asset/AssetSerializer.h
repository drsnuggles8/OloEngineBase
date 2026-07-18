#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <filesystem>
#include <optional>
#include <variant>
#include "AssetMetadata.h"
#include "MeshColliderAsset.h"

#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Renderer/GPUResourceQueue.h"   // For RawTextureData, RawShaderData
#include "OloEngine/Renderer/TextureCompression.h" // For CompressedTextureImage (.olotex)

// Forward declarations
namespace YAML
{
    class Node;
}

namespace OloEngine
{
    // Forward declarations
    class MaterialAsset;
    class PhysicsMaterial;
    class Prefab;
    class Scene;
    class ScriptFileAsset;
    class SoundConfigAsset;
    class AnimationAsset;
    class AnimationGraphAsset;
    class SoundGraphAsset;

    class LightProbeVolumeAsset;
    class ParticleSystemAsset;
    class DialogueTreeAsset;
    class BehaviorTreeAsset;
    class StateMachineAsset;
    class CinematicSequence;
    class ExperienceCurve;
    class SkillTreeDatabase;
    class CharacterClassDatabase;

    struct AssetSerializationInfo
    {
        u64 Offset = 0;
        u64 Size = 0;
    };

    /**
     * @brief Variant type for raw asset data loaded from disk (no GPU resources)
     *
     * This is used for async loading where worker threads load data from disk
     * and main thread finalizes GPU resources.
     */
    using RawAssetData = std::variant<
        std::monostate,        // Empty/invalid
        RawTextureData,        // Decoded pixel data
        RawShaderData,         // Shader source code
        CompressedTextureImage // Offline BCn mip chain (.olotex, #440)
        // Add more types as needed (RawMeshData, etc.)
        >;

    class AssetSerializer
    {
      public:
        virtual ~AssetSerializer() noexcept = default;

        virtual void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const = 0;
        [[nodiscard]] virtual bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const = 0;
        virtual void RegisterDependencies([[maybe_unused]] const AssetMetadata& metadata) const {}

        /**
         * @brief Load raw asset data from disk without creating GPU resources
         *
         * This method is safe to call from worker threads. It loads and decodes
         * file data but does NOT create any GPU resources (textures, shaders, etc.)
         *
         * @param metadata Asset metadata
         * @param outRawData Output raw data (variant type)
         * @return True if raw data was loaded successfully
         *
         * Default implementation returns false (not supported).
         * Override in serializers that support async loading.
         */
        [[nodiscard]] virtual bool TryLoadRawData([[maybe_unused]] const AssetMetadata& metadata,
                                                  [[maybe_unused]] RawAssetData& outRawData) const
        {
            return false; // Not supported by default
        }

        /**
         * @brief Create an asset from previously loaded raw data
         *
         * This method MUST be called from the main thread as it creates GPU resources.
         *
         * @param rawData Raw data loaded via TryLoadRawData
         * @param asset Output asset reference
         * @return True if asset was created successfully
         *
         * Default implementation returns false (not supported).
         */
        [[nodiscard]] virtual bool FinalizeFromRawData([[maybe_unused]] const RawAssetData& rawData,
                                                       [[maybe_unused]] Ref<Asset>& asset) const
        {
            return false; // Not supported by default
        }

        /**
         * @brief Check if this serializer supports async (two-phase) loading
         */
        [[nodiscard]] virtual bool SupportsAsyncLoading() const
        {
            return false;
        }

        /**
         * @brief Whether DeserializeFromAssetPack may be called off the main thread
         *
         * The runtime async asset system fully deserializes packed assets on a worker
         * thread. That is only safe for serializers whose DeserializeFromAssetPack does
         * no GPU / main-thread-only work (e.g. no Texture2D/Mesh/Shader/Font creation).
         *
         * Defaults to false — the conservative, always-correct choice: the runtime
         * falls back to synchronous (main-thread) loading. Override to true only for
         * CPU-only serializers.
         */
        [[nodiscard]] virtual bool CanDeserializeFromAssetPackOffThread() const
        {
            return false;
        }

        [[nodiscard]] virtual bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const = 0;
        virtual Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const = 0;

        // Virtual method for scene-specific deserialization, returns nullptr by
        // default. Defined out-of-line (AssetSerializer.cpp) on purpose: an
        // inline body returning Ref<Scene> would force every TU that includes
        // this header to see the complete Scene type just to instantiate
        // Ref<Scene>::~Ref.
        virtual Ref<Scene> DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& sceneInfo) const;
    };

    class TextureSerializer : public AssetSerializer
    {
      public:
        void Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const override {}
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        // Two-phase async loading support
        [[nodiscard]] bool TryLoadRawData(const AssetMetadata& metadata, RawAssetData& outRawData) const override;
        [[nodiscard]] bool FinalizeFromRawData(const RawAssetData& rawData, Ref<Asset>& asset) const override;
        [[nodiscard]] bool SupportsAsyncLoading() const override
        {
            return true;
        }

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

        // Filename-based sRGB heuristic for the path-load path. The model
        // loaders (Model.cpp / AnimatedModel.cpp) pick sRGB explicitly per
        // aiTextureType, but standalone .png drag-drops through the asset
        // pipeline only see a path — use the filename to decide whether the
        // bytes encode colour (albedo / emissive) or linear data (normal /
        // metallic / roughness / AO / height). Returns true for colour.
        [[nodiscard]] static bool IsLikelyColorTextureByName(std::string_view filename);

        // Asset-pack cook policy (#440). When enabled, SerializeToAssetPack BC-compresses
        // an uncompressed source texture (BC7 for colour/linear, BC6H for HDR) into an
        // embedded .olotex instead of shipping the raw image path. AssetPackBuilder sets
        // this from BuildSettings::m_CompressAssets around a pack build (single-threaded;
        // reset to false afterwards). Already-compressed (.olotex) textures are unaffected.
        static void SetAssetPackCompressionEnabled(bool enabled);
        [[nodiscard]] static bool IsAssetPackCompressionEnabled();

      private:
        static std::atomic<bool> s_AssetPackCompressionEnabled;
    };

    class FontSerializer : public AssetSerializer
    {
      public:
        void Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const override {}
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class MaterialAssetSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

      private:
        std::string SerializeToYAML(Ref<MaterialAsset> materialAsset) const;
        std::string GetYAML(const AssetMetadata& metadata) const;
        void RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<MaterialAsset>& targetMaterialAsset, AssetHandle handle) const;
    };

    class EnvironmentSerializer : public AssetSerializer
    {
      public:
        void Serialize([[maybe_unused]] const AssetMetadata& metadata, [[maybe_unused]] const Ref<Asset>& asset) const override {}
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class AudioFileSourceSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        [[nodiscard]] bool CanDeserializeFromAssetPackOffThread() const override
        {
            return true;
        } // CPU-only: no GPU resources

      private:
        // Resolves a source path (already-resolved, or absent if it couldn't be
        // resolved - e.g. a relative packed path with no active project) into an
        // AudioFile: analyzes the file via AnalyzeAudioFile when present, and falls
        // back to default metadata (with a warning) when the path is absent, the file
        // is missing, or analysis fails. `fromPack` only affects log message wording.
        // Shared tail for both TryLoadData and DeserializeFromAssetPack.
        [[nodiscard]] static Ref<AudioFile> BuildAudioFileFromSource(const std::optional<std::filesystem::path>& resolvedSourcePath, AssetHandle handle, bool fromPack);

        // Analyzes an audio file via the miniaudio-backed AudioLoader and validates the
        // decoded values (finite/sane sample rate, channel count, bit depth) before
        // trusting them - a corrupt or malicious file must not propagate garbage into
        // the AudioFile asset. `fileSize` is used only as a rough duration-estimate
        // fallback for formats miniaudio can decode but can't length-query (e.g.
        // Vorbis), so a real source never reports an exact-but-wrong zero duration.
        // Returns false (out params left untouched) if the file can't be analyzed or
        // fails validation; callers should fall back to defaults.
        [[nodiscard]] static bool AnalyzeAudioFile(const std::filesystem::path& filePath, u64 fileSize, double& outDuration, u32& outSamplingRate, u16& outBitDepth, u16& outNumChannels);
    };

    class SoundConfigSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        [[nodiscard]] bool CanDeserializeFromAssetPackOffThread() const override
        {
            return true;
        } // CPU-only: YAML -> SoundConfig, no GPU resources

        // Public for testing
        std::string TestSerializeToYAML(const Ref<SoundConfigAsset>& soundConfig) const
        {
            return SerializeToYAML(soundConfig);
        }
        [[nodiscard]] bool TestDeserializeFromYAML(const std::string& yamlString, Ref<SoundConfigAsset>& soundConfig) const
        {
            return DeserializeFromYAML(yamlString, soundConfig);
        }

      private:
        std::string SerializeToYAML(const Ref<SoundConfigAsset>& soundConfig) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<SoundConfigAsset>& targetSoundConfig) const;
    };

    class PrefabSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        // NOTE: not off-thread-safe — prefab deserialization resolves referenced assets
        // (AssetManager::GetAsset<...>) which may create GPU resources.

      private:
        std::string SerializeToYAML(const Ref<Prefab>& prefab) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<Prefab>& prefab) const;
    };

    class SceneAssetSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        Ref<Scene> DeserializeSceneFromAssetPack(FileStreamReader& stream, const AssetPackFile::SceneInfo& sceneInfo) const override;
        // NOTE: not off-thread-safe — scene deserialization resolves referenced assets
        // (AssetManager::GetAsset<MeshSource/Material/Texture2D/...>) which may create
        // GPU resources, so scenes must be loaded on the main thread.

        // String serialization methods for asset pack support
        std::string SerializeToString(const Ref<Scene>& scene) const;
        [[nodiscard]] bool DeserializeFromString(const std::string& yamlString, Ref<Scene>& scene) const;
    };

    class MeshColliderSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        [[nodiscard]] bool CanDeserializeFromAssetPackOffThread() const override
        {
            return true;
        } // CPU-only: YAML -> MeshColliderAsset, no GPU resources

      private:
        std::string SerializeToYAML(Ref<MeshColliderAsset> meshCollider) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<MeshColliderAsset> targetMeshCollider) const;
        void RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const;
    };

    class ScriptFileSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        [[nodiscard]] bool CanDeserializeFromAssetPackOffThread() const override
        {
            return true;
        } // CPU-only: text -> ScriptFileAsset, no GPU resources

      private:
        std::string SerializeToYAML(Ref<ScriptFileAsset> scriptAsset) const;
        bool DeserializeFromYAML(const std::string& yamlString, Ref<ScriptFileAsset> targetScriptAsset) const;
    };

    // Missing serializers that Hazel has
    class MeshSourceSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override
        {
            (void)metadata;
            (void)asset;
        }
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class MeshSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class StaticMeshSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class AnimationAssetSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

      private:
        std::string SerializeToYAML(Ref<AnimationAsset> animationAsset) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<AnimationAsset>& animationAsset) const;
        [[nodiscard]] bool DeserializeFromYAML(const YAML::Node& data, Ref<AnimationAsset>& animationAsset) const; // For pre-parsed YAML nodes
        void RegisterDependenciesFromYAML(const std::string& yamlString, AssetHandle handle) const;
    };

    class AnimationGraphAssetSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class CinematicSequenceAssetSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class FluidSettingsAssetSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;
        void RegisterDependencies(const AssetMetadata& metadata) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class SoundGraphSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class ParticleSystemAssetSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

      private:
        std::string SerializeToYAML(const Ref<ParticleSystemAsset>& particleAsset) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<ParticleSystemAsset>& particleAsset) const;
    };

    class LightProbeVolumeSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
    };

    class DialogueTreeSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

        // Public for testing
        std::string TestSerializeToYAML(const Ref<DialogueTreeAsset>& dialogueAsset) const
        {
            return SerializeToYAML(dialogueAsset);
        }
        [[nodiscard]] bool TestDeserializeFromYAML(const std::string& yamlString, Ref<DialogueTreeAsset>& dialogueAsset) const
        {
            return DeserializeFromYAML(yamlString, dialogueAsset);
        }

      private:
        std::string SerializeToYAML(const Ref<DialogueTreeAsset>& dialogueAsset) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<DialogueTreeAsset>& dialogueAsset) const;
    };

    class BehaviorTreeSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

      private:
        std::string SerializeToYAML(const Ref<BehaviorTreeAsset>& btAsset) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<BehaviorTreeAsset>& btAsset) const;
    };

    class StateMachineSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

      private:
        std::string SerializeToYAML(const Ref<StateMachineAsset>& fsmAsset) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<StateMachineAsset>& fsmAsset) const;
    };

    class InstancePlacementAsset; // Forward declaration

    class InstancePlacementSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

      private:
        std::string SerializeToYAML(const Ref<InstancePlacementAsset>& asset) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<InstancePlacementAsset>& asset) const;
    };

    class ShaderGraphAsset; // Forward declaration

    class ShaderGraphSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;

        // Public for testing
        std::string TestSerializeToYAML(const Ref<ShaderGraphAsset>& graphAsset) const
        {
            return SerializeToYAML(graphAsset);
        }
        [[nodiscard]] bool TestDeserializeFromYAML(const std::string& yamlString, Ref<ShaderGraphAsset>& graphAsset) const
        {
            return DeserializeFromYAML(yamlString, graphAsset);
        }

      private:
        std::string SerializeToYAML(const Ref<ShaderGraphAsset>& graphAsset) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<ShaderGraphAsset>& graphAsset) const;
    };

    // Progression data assets (issue #635). All three are CPU-only YAML value
    // databases; implementations live in
    // OloEngine/Gameplay/Progression/ProgressionAssetSerializers.cpp.

    class ExperienceCurveSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        [[nodiscard]] bool CanDeserializeFromAssetPackOffThread() const override
        {
            return true; // CPU-only: YAML -> ExperienceCurve, no GPU resources
        }

        // Public for testing
        std::string TestSerializeToYAML(const Ref<ExperienceCurve>& curve) const
        {
            return SerializeToYAML(curve);
        }
        [[nodiscard]] bool TestDeserializeFromYAML(const std::string& yamlString, Ref<ExperienceCurve>& curve) const
        {
            return DeserializeFromYAML(yamlString, curve);
        }

      private:
        std::string SerializeToYAML(const Ref<ExperienceCurve>& curve) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<ExperienceCurve>& curve) const;
    };

    class SkillTreeDatabaseSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        [[nodiscard]] bool CanDeserializeFromAssetPackOffThread() const override
        {
            return true; // CPU-only: YAML -> SkillTreeDatabase, no GPU resources
        }

        // Public for testing
        std::string TestSerializeToYAML(const Ref<SkillTreeDatabase>& tree) const
        {
            return SerializeToYAML(tree);
        }
        [[nodiscard]] bool TestDeserializeFromYAML(const std::string& yamlString, Ref<SkillTreeDatabase>& tree) const
        {
            return DeserializeFromYAML(yamlString, tree);
        }

      private:
        std::string SerializeToYAML(const Ref<SkillTreeDatabase>& tree) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<SkillTreeDatabase>& tree) const;
    };

    class CharacterClassDatabaseSerializer : public AssetSerializer
    {
      public:
        void Serialize(const AssetMetadata& metadata, const Ref<Asset>& asset) const override;
        [[nodiscard]] bool TryLoadData(const AssetMetadata& metadata, Ref<Asset>& asset) const override;

        [[nodiscard]] bool SerializeToAssetPack(AssetHandle handle, FileStreamWriter& stream, AssetSerializationInfo& outInfo) const override;
        Ref<Asset> DeserializeFromAssetPack(FileStreamReader& stream, const AssetPackFile::AssetInfo& assetInfo) const override;
        [[nodiscard]] bool CanDeserializeFromAssetPackOffThread() const override
        {
            return true; // CPU-only: YAML -> CharacterClassDatabase, no GPU resources
        }

        // Public for testing
        std::string TestSerializeToYAML(const Ref<CharacterClassDatabase>& classDb) const
        {
            return SerializeToYAML(classDb);
        }
        [[nodiscard]] bool TestDeserializeFromYAML(const std::string& yamlString, Ref<CharacterClassDatabase>& classDb) const
        {
            return DeserializeFromYAML(yamlString, classDb);
        }

      private:
        std::string SerializeToYAML(const Ref<CharacterClassDatabase>& classDb) const;
        [[nodiscard]] bool DeserializeFromYAML(const std::string& yamlString, Ref<CharacterClassDatabase>& classDb) const;
    };

} // namespace OloEngine
