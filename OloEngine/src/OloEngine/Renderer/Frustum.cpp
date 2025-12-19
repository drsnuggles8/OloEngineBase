#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/BoundingVolume.h"

namespace OloEngine
{
    Frustum::Frustum(const glm::mat4& viewProjection)
    {
        Update(viewProjection);
    }

    void Frustum::Update(const glm::mat4& viewProjection)
    {
        using enum OloEngine::Frustum::Planes;
        OLO_PROFILE_FUNCTION();

        // Mapping of plane indices to the corresponding matrix row operations
        constexpr std::array<std::tuple<Planes, int, int>, 6> planeData = { { { Left, 0, 0 }, { Right, 0, 0 }, { Bottom, 1, 1 }, { Top, 1, 1 }, { Near, 2, 2 }, { Far, 2, 2 } } };

        for (const auto& [plane, row, col] : planeData)
        {
            auto idx = static_cast<sizet>(std::to_underlying(plane));
            float sign = (plane == Right || plane == Top || plane == Far) ? -1.0f : 1.0f;

            m_Planes[idx].Normal.x = viewProjection[0][3] + sign * viewProjection[0][col];
            m_Planes[idx].Normal.y = viewProjection[1][3] + sign * viewProjection[1][col];
            m_Planes[idx].Normal.z = viewProjection[2][3] + sign * viewProjection[2][col];
            m_Planes[idx].Distance = viewProjection[3][3] + sign * viewProjection[3][col];
        }

        // Normalize all planes
        for (auto& plane : m_Planes)
        {
            float length = glm::length(plane.Normal);
            plane.Normal /= length;
            plane.Distance /= length;
        }
    }

    bool Frustum::IsPointVisible(const glm::vec3& point) const
    {
        return std::ranges::all_of(m_Planes, [&](const auto& plane)
                                   { return plane.GetSignedDistance(point) >= 0.0f; });
    }

    bool Frustum::IsSphereVisible(const glm::vec3& center, f32 radius) const
    {
        return std::ranges::all_of(m_Planes, [&](const auto& plane)
                                   { return plane.GetSignedDistance(center) >= -radius; });
    }

    bool Frustum::IsBoundingSphereVisible(const BoundingSphere& sphere) const
    {
        return IsSphereVisible(sphere.Center, sphere.Radius);
    }

    bool Frustum::IsBoxVisible(const glm::vec3& min, const glm::vec3& max) const
    {
        for (const auto& plane : m_Planes)
        {
            glm::vec3 p = min;
            if (plane.Normal.x >= 0.0f)
                p.x = max.x;
            if (plane.Normal.y >= 0.0f)
                p.y = max.y;
            if (plane.Normal.z >= 0.0f)
                p.z = max.z;

            if (plane.GetSignedDistance(p) < 0.0f)
                return false;
        }
        return true;
    }

    bool Frustum::IsBoundingBoxVisible(const BoundingBox& box) const
    {
        return IsBoxVisible(box.Min, box.Max);
    }
} // namespace OloEngine
