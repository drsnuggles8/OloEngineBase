#pragma once

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneBinaryIO.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/AnimatedModel.h"

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
        // Scene YAML schema version. Bump this and add a migration step to
        // MigrateSceneYAML (in SceneSerializer.cpp) whenever a change to the
        // on-disk format needs one. Every scene file written before this
        // versioning scheme existed has no "Version" key at all; those are
        // treated as ImplicitVersion so the entire existing scene library
        // keeps loading unmodified.
        static constexpr u32 CurrentVersion = 1;
        static constexpr u32 ImplicitVersion = 0;

        explicit SceneSerializer(const Ref<Scene>& scene);

        void Serialize(const std::filesystem::path& filepath) const;
        [[maybe_unused]] void SerializeRuntime(const std::filesystem::path& filepath) const;

        [[nodiscard("Store this!")]] bool Deserialize(const std::filesystem::path& filepath);
        [[nodiscard("Store this!")]] [[maybe_unused]] bool DeserializeRuntime(const std::filesystem::path& filepath);

        // String-based serialization methods for asset pack support
        std::string SerializeToYAML() const;
        bool DeserializeFromYAML(const std::string& yamlString);

        // Additive deserialization: merge entities from YAML node into existing scene
        // Returns UUIDs of all entities created
        std::vector<UUID> DeserializeAdditive(const YAML::Node& entitiesNode);

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

        // Dedup cache for AnimatedModel loads within a single Deserialize() /
        // DeserializeFromYAML() / DeserializeAdditive() call, keyed by resolved
        // absolute source path (issue #525 cheap-wins slice). Many entities in a
        // crowd scene share the same rigged-character source file; without this,
        // each one re-runs Assimp import + allocates a fresh MeshSource/GPU-buffer
        // graph. The model/mesh data is shared via Ref<AnimatedModel>; per-entity
        // playback state (AnimationStateComponent) is still populated freshly for
        // every entity, so animations stay independent per entity.
        std::unordered_map<std::string, Ref<AnimatedModel>> m_AnimatedModelCache;
    };
} // namespace OloEngine
