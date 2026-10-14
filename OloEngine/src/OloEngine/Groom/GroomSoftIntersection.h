#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    class GroomAsset;
    struct GroomCoatContext;

    struct GroomIntersectionDimensions
    {
        f32 MeanDiameter = 0.0f;
        f32 MeanLength = 0.0f;
        u32 SampledCurves = 0;
        glm::mat3 TransformMetric{ 1.0f };
    };

    // Uses the production rest-shaping walk on the base fibres, without card
    // widening or LOD compensation. Cache this with the geometry/coat digest.
    [[nodiscard]] GroomIntersectionDimensions MeasureGroomIntersectionDimensions(
        const GroomAsset& groom, const GroomCoatContext* coat, const glm::mat3& transform = glm::mat3(1.0f));

    // Translation and rigid rotation preserve length. Compare the metric
    // rather than the model matrix to avoid a new shaping walk during motion.
    [[nodiscard]] inline bool GroomIntersectionMetricMatches(const GroomIntersectionDimensions& dimensions,
                                                             const glm::mat3& transform) noexcept
    {
        const glm::mat3 metric = glm::transpose(transform) * transform;
        for (u32 column = 0; column < 3u; ++column)
            for (u32 row = 0; row < 3u; ++row)
            {
                const f32 a = metric[column][row];
                const f32 b = dimensions.TransformMetric[column][row];
                // Compare each axis in its own scale. A unit absolute floor
                // would treat tiny coats with very different scales as equal.
                const f32 scale = std::max({ 1.0e-12f, std::abs(a), std::abs(b) });
                const f32 roundoff = 1.0e-7f * std::sqrt(std::max(0.0f, metric[column][column] * metric[row][row]));
                if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > 0.00001f * scale + roundoff)
                    return false;
            }
        return true;
    }

    // Distances are world units. Automatic distance stays below 5% of the
    // strand length, so body-adjacent shafts beyond the protected root band
    // are not faded merely because a fine coat grows on an opaque body.
    [[nodiscard]] inline f32 GroomIntersectionDistance(f32 diameter, f32 length) noexcept
    {
        if (!std::isfinite(diameter) || !std::isfinite(length) || diameter < 0.0f || length <= 0.0f)
            return 0.0f;
        return std::min(std::max(2.0f * diameter, 0.02f * length), 0.05f * length);
    }

    [[nodiscard]] inline f32 GroomIntersectionCoverage(f32 separation, f32 distance,
                                                       f32 curveParameter, f32 protectedRoot) noexcept
    {
        if (!std::isfinite(distance) || distance <= 0.0f)
            return 1.0f;
        if (!std::isfinite(separation) || !std::isfinite(curveParameter) || !std::isfinite(protectedRoot))
            return 1.0f;
        const f32 root = std::clamp(protectedRoot, 0.0f, 1.0f);
        if (root >= 1.0f || (root > 0.0f && curveParameter <= root))
            return 1.0f;
        const f32 transition = std::min(root, 1.0f - root);
        const f32 t = transition > 0.0f ? std::clamp((curveParameter - root) / transition, 0.0f, 1.0f)
                                        : (curveParameter < root ? 0.0f : 1.0f);
        const f32 weight = t * t * (3.0f - 2.0f * t);
        return 1.0f + weight * (std::clamp(separation / distance, 0.0f, 1.0f) - 1.0f);
    }
} // namespace OloEngine
