#pragma once

#include "OloEngine/Containers/Array.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    // Bound GPU upload/dispatch work without losing any changed footprint.
    // maxFootprints is positive; entries are stored as consecutive min/max pairs.
    inline void AppendBoundedShadowInvalidation(TArray64<glm::vec4>& queue,
                                                const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                                                u32 maxFootprints)
    {
        if (static_cast<sizet>(queue.Num()) >= 2 * static_cast<sizet>(maxFootprints))
        {
            auto& lastMin = queue[queue.Num() - 2];
            auto& lastMax = queue[queue.Num() - 1];
            lastMin = glm::vec4(glm::min(glm::vec3(lastMin), boundsMin), 0.0f);
            lastMax = glm::vec4(glm::max(glm::vec3(lastMax), boundsMax), 0.0f);
            return;
        }
        queue.Emplace_GetRef(boundsMin, 0.0f);
        queue.Emplace_GetRef(boundsMax, 0.0f);
    }
} // namespace OloEngine
