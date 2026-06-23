#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine::PerceptionMath
{
    // Returns true if `targetPos` lies inside the sight cone of an observer at
    // `eye` looking along unit vector `forward`. A point is visible when it is
    // within `range` AND the angle between `forward` and the direction to the
    // target is at most half of `fovDegrees` (the full angular width of the
    // cone). The half-angle test is done in cosine space —
    //   dot(forward, dir) >= cos(radians(fovDegrees / 2))
    // — which is the standard cone-membership predicate (larger dot product =
    // smaller angle). A target essentially on top of the eye has no defined
    // direction and is treated as inside the cone. `forward` is normalized
    // defensively so callers may pass an un-normalized look vector. This is the
    // single source of truth for the range+FOV gate; PerceptionSystem and its
    // unit tests both call it.
    [[nodiscard("sight-cone membership result must be used")]] inline bool IsInSightCone(const glm::vec3& eye, const glm::vec3& forward,
                                                                                         const glm::vec3& targetPos, f32 range, f32 fovDegrees)
    {
        const glm::vec3 toTarget = targetPos - eye;
        const f32 distSq = glm::dot(toTarget, toTarget);
        if (distSq > (range * range))
        {
            return false; // out of range
        }

        const f32 dist = std::sqrt(distSq);
        if (dist <= 0.0001f)
        {
            return true; // coincident with the eye → inside the cone
        }

        const glm::vec3 dirToTarget = toTarget / dist;
        const f32 halfFovCos = std::cos(glm::radians(std::clamp(fovDegrees, 0.0f, 360.0f) * 0.5f));
        return glm::dot(glm::normalize(forward), dirToTarget) >= halfFovCos;
    }
} // namespace OloEngine::PerceptionMath
