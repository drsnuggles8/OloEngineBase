#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <optional>

namespace OloEngine
{
    // The height field and every streamed tile share this terrain-local coordinate system.
    struct TerrainFrame
    {
        glm::mat4 Model{ 1.0f };

        [[nodiscard]] glm::mat4 TileModel(const glm::vec3& localOrigin) const
        {
            return glm::translate(Model, localOrigin);
        }

        [[nodiscard]] std::optional<glm::vec3> ToLocal(const glm::vec3& world) const
        {
            const glm::vec3 point(glm::inverse(Model) * glm::vec4(world, 1.0f));
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
                return std::nullopt;
            return point;
        }

        // A single-valued, axis-aligned world-XZ height lookup supports a
        // yawed, scaled or mirrored terrain, including sloping height samples.
        // A pitched local height axis requires a ray/surface intersection.
        [[nodiscard]] bool SupportsVerticalHeight() const
        {
            const glm::vec3 up(Model[1]);
            return std::hypot(up.x, up.z) <= 1.0e-6f * std::abs(up.y) && std::abs(up.y) > 0.0f;
        }
    };
} // namespace OloEngine
