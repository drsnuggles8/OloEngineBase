#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine::FluidShading
{
    // @brief Beer–Lambert transmittance through `thickness` metres of fluid.
    //
    //   T[c] = exp(-absorptionColor[c] * absorptionScale * thickness)
    //
    // GLSL mirror: OloEditor/assets/shaders/FluidComposite.glsl (fragment
    // stage, "Beer–Lambert absorption") — keep formula-identical. The shader
    // omits the sanitisation below because the pass upload path validates the
    // parameters CPU-side before they reach the FluidRenderUBO.
    //
    // Guards (pinned by FluidRenderMathTest):
    //  - thickness <= 0 or non-finite            → vec3(1) (no absorption)
    //  - absorptionScale <= 0 or non-finite      → vec3(1)
    //  - a negative / non-finite colour channel  → that channel absorbs nothing
    //
    // Channels are independent: T[c] depends only on absorptionColor[c].
    [[nodiscard]] inline glm::vec3 Transmittance(const glm::vec3& absorptionColor, f32 absorptionScale, f32 thickness)
    {
        if (!std::isfinite(thickness) || thickness <= 0.0f ||
            !std::isfinite(absorptionScale) || absorptionScale <= 0.0f)
        {
            return glm::vec3(1.0f);
        }

        glm::vec3 sanitized(0.0f);
        for (glm::length_t c = 0; c < 3; ++c)
        {
            const f32 channel = absorptionColor[c];
            if (std::isfinite(channel) && channel > 0.0f)
                sanitized[c] = channel;
        }

        return glm::exp(-sanitized * absorptionScale * thickness);
    }
} // namespace OloEngine::FluidShading
