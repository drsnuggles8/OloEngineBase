#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"

#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    // Unique identifier for a zone.
    using ZoneID = u32;

    // Axis-aligned bounding box for zone boundaries.
    struct ZoneBounds
    {
        glm::vec3 Min{ 0.0f };
        glm::vec3 Max{ 0.0f };

        [[nodiscard]] bool Contains(const glm::vec3& point) const
        {
            return point.x >= Min.x && point.x <= Max.x && point.y >= Min.y && point.y <= Max.y && point.z >= Min.z && point.z <= Max.z;
        }
    };

    // Describes a zone in the world — maps to a streaming region.
    struct ZoneDefinition
    {
        ZoneID ID = 0;
        std::string Name;
        AssetHandle RegionAssetHandle = 0; // Links to a .oloregion file
        ZoneBounds Bounds;
        u32 MaxPlayers = 200;
        u32 TickRateHz = 20;
    };
} // namespace OloEngine
