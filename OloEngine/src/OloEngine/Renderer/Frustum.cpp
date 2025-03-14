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
        OLO_PROFILE_FUNCTION();

        // Extract the planes from the view-projection matrix
        // Based on the method described in "Fast Extraction of Viewing Frustum Planes"

        // Left plane
        m_Planes[static_cast<size_t>(Planes::Left)].Normal.x = viewProjection[0][3] + viewProjection[0][0];
        m_Planes[static_cast<size_t>(Planes::Left)].Normal.y = viewProjection[1][3] + viewProjection[1][0];
        m_Planes[static_cast<size_t>(Planes::Left)].Normal.z = viewProjection[2][3] + viewProjection[2][0];
        m_Planes[static_cast<size_t>(Planes::Left)].Distance = viewProjection[3][3] + viewProjection[3][0];

        // Right plane
        m_Planes[static_cast<size_t>(Planes::Right)].Normal.x = viewProjection[0][3] - viewProjection[0][0];
        m_Planes[static_cast<size_t>(Planes::Right)].Normal.y = viewProjection[1][3] - viewProjection[1][0];
        m_Planes[static_cast<size_t>(Planes::Right)].Normal.z = viewProjection[2][3] - viewProjection[2][0];
        m_Planes[static_cast<size_t>(Planes::Right)].Distance = viewProjection[3][3] - viewProjection[3][0];

        // Bottom plane
        m_Planes[static_cast<size_t>(Planes::Bottom)].Normal.x = viewProjection[0][3] + viewProjection[0][1];
        m_Planes[static_cast<size_t>(Planes::Bottom)].Normal.y = viewProjection[1][3] + viewProjection[1][1];
        m_Planes[static_cast<size_t>(Planes::Bottom)].Normal.z = viewProjection[2][3] + viewProjection[2][1];
        m_Planes[static_cast<size_t>(Planes::Bottom)].Distance = viewProjection[3][3] + viewProjection[3][1];

        // Top plane
        m_Planes[static_cast<size_t>(Planes::Top)].Normal.x = viewProjection[0][3] - viewProjection[0][1];
        m_Planes[static_cast<size_t>(Planes::Top)].Normal.y = viewProjection[1][3] - viewProjection[1][1];
        m_Planes[static_cast<size_t>(Planes::Top)].Normal.z = viewProjection[2][3] - viewProjection[2][1];
        m_Planes[static_cast<size_t>(Planes::Top)].Distance = viewProjection[3][3] - viewProjection[3][1];

        // Near plane
        m_Planes[static_cast<size_t>(Planes::Near)].Normal.x = viewProjection[0][3] + viewProjection[0][2];
        m_Planes[static_cast<size_t>(Planes::Near)].Normal.y = viewProjection[1][3] + viewProjection[1][2];
        m_Planes[static_cast<size_t>(Planes::Near)].Normal.z = viewProjection[2][3] + viewProjection[2][2];
        m_Planes[static_cast<size_t>(Planes::Near)].Distance = viewProjection[3][3] + viewProjection[3][2];

        // Far plane
        m_Planes[static_cast<size_t>(Planes::Far)].Normal.x = viewProjection[0][3] - viewProjection[0][2];
        m_Planes[static_cast<size_t>(Planes::Far)].Normal.y = viewProjection[1][3] - viewProjection[1][2];
        m_Planes[static_cast<size_t>(Planes::Far)].Normal.z = viewProjection[2][3] - viewProjection[2][2];
        m_Planes[static_cast<size_t>(Planes::Far)].Distance = viewProjection[3][3] - viewProjection[3][2];

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
        for (const auto& plane : m_Planes)
        {
            if (plane.GetSignedDistance(point) < 0.0f)
                return false;
        }
        return true;
    }

    bool Frustum::IsSphereVisible(const glm::vec3& center, float radius) const
    {
        for (const auto& plane : m_Planes)
        {
            if (plane.GetSignedDistance(center) < -radius)
                return false;
        }
        return true;
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
            if (plane.Normal.x >= 0.0f) p.x = max.x;
            if (plane.Normal.y >= 0.0f) p.y = max.y;
            if (plane.Normal.z >= 0.0f) p.z = max.z;

            if (plane.GetSignedDistance(p) < 0.0f)
                return false;
        }
        return true;
    }

    bool Frustum::IsBoundingBoxVisible(const BoundingBox& box) const
    {
        return IsBoxVisible(box.Min, box.Max);
    }
} 