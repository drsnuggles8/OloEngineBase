#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    // Axis-aligned bounding box
    struct BoundingBox
    {
        glm::vec3 Min;
        glm::vec3 Max;
        
        BoundingBox() = default;
        
        BoundingBox(const glm::vec3& min, const glm::vec3& max)
            : Min(min), Max(max) {}
        
        // Create a bounding box from an array of points
        BoundingBox(const glm::vec3* points, size_t count)
        {
            if (count == 0)
            {
                Min = glm::vec3(0.0f);
                Max = glm::vec3(0.0f);
                return;
            }
            
            Min = points[0];
            Max = points[0];
            
            for (size_t i = 1; i < count; ++i)
            {
                Min = glm::min(Min, points[i]);
                Max = glm::max(Max, points[i]);
            }
        }
        
        // Get the center of the box
        [[nodiscard]] glm::vec3 GetCenter() const
        {
            return (Min + Max) * 0.5f;
        }
        
        // Get the size of the box
        [[nodiscard]] glm::vec3 GetSize() const
        {
            return Max - Min;
        }
        
        // Get the extents (half-size) of the box
        [[nodiscard]] glm::vec3 GetExtents() const
        {
            return GetSize() * 0.5f;
        }
        
        // Transform the bounding box by a matrix
        [[nodiscard]] BoundingBox Transform(const glm::mat4& transform) const
        {
            // Get the 8 corners of the box
            glm::vec3 corners[8];
            corners[0] = glm::vec3(Min.x, Min.y, Min.z);
            corners[1] = glm::vec3(Max.x, Min.y, Min.z);
            corners[2] = glm::vec3(Min.x, Max.y, Min.z);
            corners[3] = glm::vec3(Max.x, Max.y, Min.z);
            corners[4] = glm::vec3(Min.x, Min.y, Max.z);
            corners[5] = glm::vec3(Max.x, Min.y, Max.z);
            corners[6] = glm::vec3(Min.x, Max.y, Max.z);
            corners[7] = glm::vec3(Max.x, Max.y, Max.z);
            
            // Transform all corners
            for (auto& corner : corners)
            {
                glm::vec4 transformedCorner = transform * glm::vec4(corner, 1.0f);
                corner = glm::vec3(transformedCorner) / transformedCorner.w;
            }
            
            // Create a new bounding box from the transformed corners
            return BoundingBox(corners, 8);
        }
    };
    
    // Bounding sphere
    struct BoundingSphere
    {
        glm::vec3 Center;
        float Radius;
        
        BoundingSphere() = default;
        
        BoundingSphere(const glm::vec3& center, float radius)
            : Center(center), Radius(radius) {}
        
        // Create a bounding sphere from an array of points
        BoundingSphere(const glm::vec3* points, size_t count)
        {
            if (count == 0)
            {
                Center = glm::vec3(0.0f);
                Radius = 0.0f;
                return;
            }
            
            // Calculate the center as the average of all points
            Center = glm::vec3(0.0f);
            for (size_t i = 0; i < count; ++i)
            {
                Center += points[i];
            }
            Center /= static_cast<float>(count);
            
            // Calculate the radius as the maximum distance from the center
            Radius = 0.0f;
            for (size_t i = 0; i < count; ++i)
            {
                float distance = glm::length(points[i] - Center);
                Radius = glm::max(Radius, distance);
            }
        }
        
        // Create a bounding sphere from a bounding box
        explicit BoundingSphere(const BoundingBox& box)
        {
            Center = box.GetCenter();
            Radius = glm::length(box.GetExtents());
        }
        
        // Transform the bounding sphere by a matrix
        [[nodiscard]] BoundingSphere Transform(const glm::mat4& transform) const
        {
            // Transform the center
            glm::vec4 transformedCenter = transform * glm::vec4(Center, 1.0f);
            glm::vec3 newCenter = glm::vec3(transformedCenter) / transformedCenter.w;
            
            // Extract the scale from the transform matrix
            glm::vec3 scale;
            scale.x = glm::length(glm::vec3(transform[0]));
            scale.y = glm::length(glm::vec3(transform[1]));
            scale.z = glm::length(glm::vec3(transform[2]));
            
            // Use the maximum scale factor for the radius with a small safety margin
            float maxScale = glm::max(glm::max(scale.x, scale.y), scale.z);
            float newRadius = Radius * maxScale * 1.05f;
            
            return BoundingSphere(newCenter, newRadius);
        }
    };
} 