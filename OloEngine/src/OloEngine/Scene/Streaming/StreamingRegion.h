#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#pragma warning(push)
#pragma warning(disable : 4275)
#include <yaml-cpp/yaml.h>
#pragma warning(pop)

namespace OloEngine
{
    class StreamingRegion : public RefCounted
    {
      public:
        enum class State : u8
        {
            Unloaded, // No data in memory
            Loading,  // Background task in flight
            Loaded,   // YAML parsed, awaiting main-thread instantiation
            Ready,    // Entities live in Scene
            Unloading // Entities being removed
        };

        StreamingRegion() = default;
        ~StreamingRegion() override = default;

        // Identity
        UUID m_RegionID;
        std::string m_Name;
        std::filesystem::path m_SourcePath; // .oloregion file

        // Spatial bounds (axis-aligned)
        glm::vec3 m_BoundsMin{ 0.0f };
        glm::vec3 m_BoundsMax{ 0.0f };

        // State (guarded by SceneStreamer::m_RegionMutex)
        State m_State = State::Unloaded;

        // LRU tracking (frame number of last proximity hit)
        u64 m_LastUsedFrame = 0;

        // Entity tracking (filled after additive deserialize)
        std::vector<UUID> m_EntityUUIDs;

        // Raw YAML data (populated by background thread, consumed on main)
        YAML::Node m_RawData;
    };
} // namespace OloEngine
