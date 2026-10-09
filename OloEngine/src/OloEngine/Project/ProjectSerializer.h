#pragma once

#include "OloEngine/Project/Project.h"

namespace YAML
{
    class Emitter;
    class Node;
} // namespace YAML

namespace OloEngine
{
    struct QualityTieringSettings;

    // The project's QualityTiering block. Shared with the shipped game's
    // manifest, so the runtime overlays the same tier the editor does (#1563).
    void SerializeQualityTiering(YAML::Emitter& out, const QualityTieringSettings& tiering);
    void DeserializeQualityTiering(const YAML::Node& node, QualityTieringSettings& tiering);

    class ProjectSerializer
    {
      public:
        ProjectSerializer(Ref<Project> project);

        bool Serialize(const std::filesystem::path& filepath);
        bool Deserialize(const std::filesystem::path& filepath);

      private:
        Ref<Project> m_Project;
    };

} // namespace OloEngine
