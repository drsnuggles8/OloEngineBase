#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine
{
    /**
     * @brief The ONE spot-light direction sanitizer.
     *
     * A zero-length or non-finite authored spot direction would make
     * glm::normalize emit NaNs in every downstream consumer (the Forward+
     * SSBO, the MultiLight UBO, the spot-shadow projection, and the reference
     * path tracer's ReferenceLight); fall back to a safe -Z unit instead.
     * Valid directions pass through UNCHANGED — unnormalized, exactly as
     * authored — because the consumers normalize themselves (the shaders, and
     * ReferenceBRDF::CalculateSpotIntensity, which is a port of them).
     *
     * This lives in one place because Scene.cpp's light packing and
     * ReferenceSceneBuilder's light mirroring must agree BIT-FOR-BIT: the
     * reference path tracer's raster-vs-reference comparisons (and the #439
     * bake) are only about transport if the two paths pack the same light.
     * Do not change the fallback or the 1e-8 floor in one consumer without
     * the other — that is the drift this header exists to prevent.
     */
    [[nodiscard]] inline glm::vec3 SanitizeSpotLightDirection(const glm::vec3& dir)
    {
        const f32 len2 = glm::dot(dir, dir);
        if (!std::isfinite(len2) || len2 < 1e-8f)
        {
            return glm::vec3(0.0f, 0.0f, -1.0f);
        }
        return dir;
    }
} // namespace OloEngine
