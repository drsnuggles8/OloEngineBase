#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>
#include <array>

namespace OloEngine
{
    // Forward declarations
    struct BoundingBox;
    struct BoundingSphere;

    // Represents a plane in 3D space using the equation: Ax + By + Cz + D = 0
    struct Plane
    {
        glm::vec3 Normal;  // (A, B, C) components - normalized
        float Distance;    // D component

        Plane() = default;
        Plane(const glm::vec3& normal, float distance)
            : Normal(normal), Distance(distance) {}

        // Create a plane from three points
        Plane(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
        {
            Normal = glm::normalize(glm::cross(b - a, c - a));
            Distance = -glm::dot(Normal, a);
        }

        // Create a plane from normal and point
        Plane(const glm::vec3& normal, const glm::vec3& point)
            : Normal(glm::normalize(normal))
        {
            Distance = -glm::dot(Normal, point);
        }

        // Get signed distance from point to plane
        float GetSignedDistance(const glm::vec3& point) const
        {
            return glm::dot(Normal, point) + Distance;
        }
    };

    // Represents a view frustum with 6 planes
    class Frustum
    {
    public:
        enum class Planes
        {
            Near = 0,
            Far,
            Left,
            Right,
            Top,
            Bottom,
            Count
        };

        Frustum() = default;

        // Construct frustum from view-projection matrix
        explicit Frustum(const glm::mat4& viewProjection);

        // Update frustum planes from view-projection matrix
        void Update(const glm::mat4& viewProjection);

        // Test if point is inside frustum
        [[nodiscard]] bool IsPointVisible(const glm::vec3& point) const;

        // Test if sphere is inside or intersects frustum
        [[nodiscard]] bool IsSphereVisible(const glm::vec3& center, float radius) const;

        // Test if bounding sphere is inside or intersects frustum
        [[nodiscard]] bool IsBoundingSphereVisible(const BoundingSphere& sphere) const;

        // Test if axis-aligned bounding box is inside or intersects frustum
        [[nodiscard]] bool IsBoxVisible(const glm::vec3& min, const glm::vec3& max) const;

        // Test if bounding box is inside or intersects frustum
        [[nodiscard]] bool IsBoundingBoxVisible(const BoundingBox& box) const;

        // Get a specific plane
        [[nodiscard]] const Plane& GetPlane(Planes plane) const { return m_Planes[static_cast<size_t>(plane)]; }

    private:
        std::array<Plane, static_cast<size_t>(Planes::Count)> m_Planes;
    };
} 