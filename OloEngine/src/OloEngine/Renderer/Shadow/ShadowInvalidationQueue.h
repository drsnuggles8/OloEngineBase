#pragma once

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Renderer/Shadow/FoliageShadowInvalidation.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Bound GPU upload/dispatch work without losing any changed footprint.
    // maxFootprints is positive; entries are stored as consecutive min/max pairs.
    //
    // The min's w lane carries the directional CLIP LEVELS the box invalidates
    // (#1580), bit L for level L, as an exactly representable float; 0 -- what
    // every caller that names no levels passes -- is every level, as before.
    // VSM_InvalidatePages.comp reads it; the local-light half ignores it. A
    // saturated queue merges into its last box and invalidates the union of
    // both boxes' levels (FoliageShadowInvalidation::MergeLevelMasks).
    inline void AppendBoundedShadowInvalidation(TArray64<glm::vec4>& queue,
                                                const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                                                u32 maxFootprints, u32 clipLevelMask = FoliageShadowInvalidation::kEveryLevel)
    {
        clipLevelMask &= 0xFFFFu; // 16 clip levels; keeps the float lane exact
        if (static_cast<sizet>(queue.Num()) >= 2 * static_cast<sizet>(maxFootprints))
        {
            auto& lastMin = queue[queue.Num() - 2];
            auto& lastMax = queue[queue.Num() - 1];
            const u32 merged = FoliageShadowInvalidation::MergeLevelMasks(static_cast<u32>(lastMin.w), clipLevelMask);
            lastMin = glm::vec4(glm::min(glm::vec3(lastMin), boundsMin), static_cast<f32>(merged));
            lastMax = glm::vec4(glm::max(glm::vec3(lastMax), boundsMax), 0.0f);
            return;
        }
        queue.Emplace_GetRef(boundsMin, static_cast<f32>(clipLevelMask));
        queue.Emplace_GetRef(boundsMax, 0.0f);
    }
} // namespace OloEngine
