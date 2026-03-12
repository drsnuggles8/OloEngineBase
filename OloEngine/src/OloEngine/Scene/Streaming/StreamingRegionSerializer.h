#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "StreamingRegion.h"

#include <filesystem>

#pragma warning(push)
#pragma warning(disable : 4275)
#include <yaml-cpp/yaml.h>
#pragma warning(pop)

#include <glm/glm.hpp>

namespace OloEngine
{
    class Scene;

    class StreamingRegionSerializer
    {
      public:
        explicit StreamingRegionSerializer(const Ref<Scene>& scene);

        // Write region to disk (.oloregion)
        void Serialize(const Ref<StreamingRegion>& region, const std::filesystem::path& path) const;

        // Background-thread safe: file I/O + YAML parse only.
        // Does NOT touch Scene/ECS.
        static YAML::Node ParseRegionFile(const std::filesystem::path& path);

        // Extract metadata from parsed node without full deserialize
        struct RegionMetadata
        {
            UUID RegionID{ 0 };
            std::string Name;
            glm::vec3 BoundsMin{ 0.0f };
            glm::vec3 BoundsMax{ 0.0f };
            u32 EntityCount = 0;
        };

        static RegionMetadata ReadMetadata(const YAML::Node& data);

      private:
        Ref<Scene> m_Scene;
    };
} // namespace OloEngine
