#pragma once

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Core/UUID.h"

#include <filesystem>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    class SceneSerializer
    {
      public:
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
        // Create entity with UUID + name, deserialize all components, and roll back
        // on failure (destroy the half-initialized entity).  Returns the new Entity
        // on success, or an invalid (null) Entity on failure.
        Entity DeserializeEntity(u64 uuid, const std::string& name, const YAML::Node& entityNode);

        Ref<Scene> m_Scene;
    };
} // namespace OloEngine
