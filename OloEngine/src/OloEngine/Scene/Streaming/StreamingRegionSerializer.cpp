#include "OloEnginePCH.h"
#include "StreamingRegionSerializer.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Core/YAMLConverters.h"

#include <fstream>

namespace OloEngine
{
    StreamingRegionSerializer::StreamingRegionSerializer(const Ref<Scene>& scene)
        : m_Scene(scene)
    {
    }

    void StreamingRegionSerializer::Serialize(const Ref<StreamingRegion>& region, const std::filesystem::path& path) const
    {
        OLO_PROFILE_FUNCTION();

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Region" << YAML::Value << region->m_Name;
        out << YAML::Key << "RegionID" << YAML::Value << static_cast<u64>(region->m_RegionID);
        out << YAML::Key << "BoundsMin" << YAML::Value << region->m_BoundsMin;
        out << YAML::Key << "BoundsMax" << YAML::Value << region->m_BoundsMax;

        out << YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;

        // Serialize each entity with ALL components via the scene serializer
        for (auto uuid : region->m_EntityUUIDs)
        {
            auto optEntity = m_Scene->TryGetEntityWithUUID(uuid);
            if (!optEntity)
            {
                continue;
            }

            SceneSerializer::SerializeEntity(out, *optEntity);
        }

        out << YAML::EndSeq;
        out << YAML::EndMap;

        std::ofstream fout(path);
        fout << out.c_str();
    }

    YAML::Node StreamingRegionSerializer::ParseRegionFile(const std::filesystem::path& path)
    {
        OLO_PROFILE_SCOPE("StreamingRegion::Parse");

        try
        {
            return YAML::LoadFile(path.string());
        }
        catch (const YAML::ParserException& e)
        {
            OLO_CORE_ERROR("Failed to parse .oloregion file '{0}'\n     {1}", path.string(), e.what());
            return {};
        }
        catch (const YAML::BadFile& e)
        {
            OLO_CORE_ERROR("Failed to open .oloregion file '{0}'\n     {1}", path.string(), e.what());
            return {};
        }
    }

    StreamingRegionSerializer::RegionMetadata StreamingRegionSerializer::ReadMetadata(const YAML::Node& data)
    {
        RegionMetadata meta;

        if (!data || !data["Region"])
        {
            return meta;
        }

        meta.Name = data["Region"].as<std::string>("");

        if (data["RegionID"])
        {
            meta.RegionID = UUID(data["RegionID"].as<u64>(0));
        }

        if (data["BoundsMin"])
        {
            meta.BoundsMin = data["BoundsMin"].as<glm::vec3>(glm::vec3{ 0.0f });
        }

        if (data["BoundsMax"])
        {
            meta.BoundsMax = data["BoundsMax"].as<glm::vec3>(glm::vec3{ 0.0f });
        }

        if (const auto entities = data["Entities"]; entities && entities.IsSequence())
        {
            meta.EntityCount = static_cast<u32>(entities.size());
        }

        return meta;
    }
} // namespace OloEngine
