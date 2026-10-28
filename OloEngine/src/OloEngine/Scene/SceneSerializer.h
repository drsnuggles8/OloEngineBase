#pragma once

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneBinaryIO.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Model.h"

#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    class SceneSerializer
    {
      public:
        // Scene YAML schema version, written as the required top-level
        // `Version` key. A scene whose Version is missing or differs from this
        // is rejected on every deserialize entry point. Bumping it means
        // migrating every scene in the repo in the same PR
        // (docs/agent-rules/binary-format-versioning.md).
        static constexpr u32 CurrentVersion = 1;

        // Optional residency across a read-only batch of scenes in ONE project
        // and content root. The caller owns this cache, outlives its serializers,
        // and clears it on the game thread before shutting down the renderer.
        // It shares model data only; entity transforms/playback remain separate.
        struct AssetLoadCache
        {
            std::unordered_map<std::string, Ref<Model>> Models;
            std::unordered_map<std::string, Ref<AnimatedModel>> AnimatedModels;

            void Clear()
            {
                Models.clear();
                AnimatedModels.clear();
            }
        };

        explicit SceneSerializer(const Ref<Scene>& scene, AssetLoadCache* sharedAssets = nullptr);

        // False (and an error in the log) when the file cannot be written. The write goes to a
        // temporary file that is renamed over the target, so a failure leaves any previous
        // save at `filepath` intact and no temporary file behind.
        [[nodiscard]] bool Serialize(const std::filesystem::path& filepath) const;
        [[maybe_unused]] void SerializeRuntime(const std::filesystem::path& filepath) const;

        [[nodiscard("Store this!")]] bool Deserialize(const std::filesystem::path& filepath);
        [[nodiscard("Store this!")]] [[maybe_unused]] bool DeserializeRuntime(const std::filesystem::path& filepath);

        // String-based serialization methods for asset pack support
        std::string SerializeToYAML() const;
        bool DeserializeFromYAML(const std::string& yamlString);

        // Additive deserialization: merge entities from YAML node into existing scene
        // Returns UUIDs of all entities created
        TArray<UUID> DeserializeAdditive(const YAML::Node& entitiesNode);

        // Serialize a single entity with all its components to a YAML emitter
        static void SerializeEntity(YAML::Emitter& out, Entity entity);

      private:
        // Apply all scene-level settings (post-process, weather, streaming) from a
        // parsed scene document node onto a scene. Shared by the YAML deserialize
        // paths and the binary sidecar fast path so scene settings have a single
        // source of truth. Never throws on malformed data (falls back to defaults
        // per-field). Defined in SceneSerializer.cpp alongside the settings helpers.
        static void ApplySceneSettings(Scene& scene, const YAML::Node& data);

        // Binary sidecar cache (issue #525). TryLoadBinarySidecar restores the
        // scene from a fresh, matching `<source>.scenebin` and returns true on the
        // fast path; on any mismatch/corruption it leaves the scene untouched and
        // returns false so the caller falls back to YAML. WriteBinarySidecar caches
        // a just-loaded scene, but only when it is fully representable in the binary
        // format (transform-only entities). Both are defined in
        // SceneBinarySerializer.cpp. `settingsData` is the parsed (post-migration)
        // scene document, used to snapshot scene-level settings into the sidecar.
        [[nodiscard]] bool TryLoadBinarySidecar(const std::filesystem::path& sourcePath);
        void WriteBinarySidecar(const std::filesystem::path& sourcePath, const YAML::Node& settingsData) const;

        // Per-entity binary component read/write for the sidecar's kBinary records.
        // Defined in SceneSerializer.cpp (this TU has every component header) and
        // splice the OloHeaderTool-generated binary blocks. CoveredComponentIds is
        // the entt::type_hash set the representability check consults.
        static void WriteEntityComponentsBinary(std::ostream& out, Entity entity);
        [[nodiscard]] static bool ReadEntityComponentsBinary(SceneBinIO::Reader& reader, Entity& deserializedEntity);
        [[nodiscard]] static const std::unordered_set<entt::id_type>& CoveredComponentIds();

        // Collect all entities sorted by UUID for deterministic serialization order.
        void ForEachEntitySorted(const std::function<void(Entity)>& fn) const;

        // Create entity with UUID + name, deserialize all components, and roll back
        // on failure (destroy the half-initialized entity).  Returns the new Entity
        // on success, or an invalid (null) Entity on failure.
        Entity DeserializeEntity(u64 uuid, const std::string& name, const YAML::Node& entityNode);

        Ref<Scene> m_Scene;
        AssetLoadCache* m_SharedAssets = nullptr; // borrowed, never process-global

        // Default dedup cache for AnimatedModel loads during this serializer's
        // lifetime (unless the caller supplies sharedAssets), keyed by resolved
        // absolute source path (issue #525 cheap-wins slice). Many entities in a
        // crowd scene share the same rigged-character source file; without this,
        // each one re-runs Assimp import + allocates a fresh MeshSource/GPU-buffer
        // graph. The model/mesh data is shared via Ref<AnimatedModel>; per-entity
        // playback state (AnimationStateComponent) is still populated freshly for
        // every entity, so animations stay independent per entity.
        std::unordered_map<std::string, Ref<AnimatedModel>> m_AnimatedModelCache;
    };
} // namespace OloEngine
