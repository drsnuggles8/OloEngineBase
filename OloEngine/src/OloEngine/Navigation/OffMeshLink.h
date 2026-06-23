#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h" // Math::BitwiseEqual

#include <glm/glm.hpp>

namespace OloEngine
{
    // A point-to-point off-mesh connection authored on a navmesh region. It lets
    // agents traverse a gap the walkable surface can't span on its own — a jump
    // across a ledge, a drop, a ladder, a teleport. During the bake
    // (NavMeshGenerator) each link is emitted as a Detour dtOffMeshConnection,
    // so NavMeshQuery::FindPath / the crowd route across it like any other edge.
    //
    // Detour gotchas (both enforced at bake time):
    //   * The Start endpoint must lie inside the bake bounds AND close to a
    //     walkable poly — within m_Radius horizontally and AgentMaxClimb
    //     vertically — or Detour silently drops the whole link.
    //   * The endpoint flags/area are fixed to the walkable poly's so the default
    //     query filter (include 0xFFFF, unit area cost) routes across them.
    struct OffMeshLink
    {
        glm::vec3 m_Start = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_End = { 0.0f, 0.0f, 0.0f };
        f32 m_Radius = 0.6f;         // endpoint connect/snap radius (world units)
        bool m_Bidirectional = true; // false = one-way, Start → End only

        OffMeshLink() = default;
        OffMeshLink(const glm::vec3& start, const glm::vec3& end, f32 radius = 0.6f, bool bidirectional = true)
            : m_Start(start), m_End(end), m_Radius(radius), m_Bidirectional(bidirectional)
        {
        }

        auto operator==(const OffMeshLink& other) const -> bool
        {
            return Math::BitwiseEqual(m_Start, other.m_Start) && Math::BitwiseEqual(m_End, other.m_End) &&
                   Math::BitwiseEqual(m_Radius, other.m_Radius) && m_Bidirectional == other.m_Bidirectional;
        }
    };
} // namespace OloEngine
