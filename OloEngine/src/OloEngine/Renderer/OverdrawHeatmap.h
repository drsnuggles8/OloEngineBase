#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>

namespace OloEngine
{
    // =========================================================================
    // Overdraw heatmap — CPU mirror of the count->colour mapping in
    //   assets/shaders/PostProcess_OverdrawHeatmap.glsl
    //
    // The overdraw debug view (issue #519) re-draws the frame's opaque geometry
    // into a single-channel accumulation target with additive blending and depth
    // testing DISABLED, so every fragment that WOULD be shaded (including those an
    // ordinary depth-tested pass would later overwrite) adds 1 to the pixel's
    // counter. This header maps that raw fragment count to a legible heat colour
    // so "how many layers deep is this frame" is visible directly.
    //
    // Both sides MUST agree so OverdrawHeatmapMathTest can pin the shader path
    // without a GL context (same contract as CASMathTest / AutoExposure.h). If you
    // change the ramp here, change the .glsl twin.
    //
    // The ramp is a simple 5-stop gradient, black -> blue -> green -> yellow ->
    // red, normalised by kOverdrawHeatmapMaxLayers. It is deliberately NOT
    // scientifically calibrated — it only needs to be monotonically "hotter"
    // (redder) as the layer count rises, and black where nothing was drawn.
    // =========================================================================
    namespace OverdrawHeatmap
    {
        // Fragment count that maps to the top of the ramp (full red). Counts at or
        // above this saturate. Chosen so typical scenes (1-3 layers) land in the
        // cool half and heavy overlap (a stack of quads, dense foliage) reads hot.
        inline constexpr f32 kOverdrawHeatmapMaxLayers = 10.0f;

        // Map a fragment count to a heat colour. count == 0 -> black; increasing
        // count walks blue -> green -> yellow -> red, saturating at maxLayers.
        // The red channel is monotonically non-decreasing in count, so a pixel with
        // more overlapping layers always reads at least as red as one with fewer.
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline glm::vec3
        HeatColor(f32 count, f32 maxLayers = kOverdrawHeatmapMaxLayers) noexcept
        {
            const f32 safeMax = maxLayers > 1e-6f ? maxLayers : 1e-6f;
            const f32 t = std::clamp(count / safeMax, 0.0f, 1.0f);

            // Five equally-spaced stops. Segment length is 0.25.
            constexpr glm::vec3 kBlack(0.0f, 0.0f, 0.0f);
            constexpr glm::vec3 kBlue(0.0f, 0.0f, 1.0f);
            constexpr glm::vec3 kGreen(0.0f, 1.0f, 0.0f);
            constexpr glm::vec3 kYellow(1.0f, 1.0f, 0.0f);
            constexpr glm::vec3 kRed(1.0f, 0.0f, 0.0f);

            if (t < 0.25f)
                return glm::mix(kBlack, kBlue, t / 0.25f);
            if (t < 0.5f)
                return glm::mix(kBlue, kGreen, (t - 0.25f) / 0.25f);
            if (t < 0.75f)
                return glm::mix(kGreen, kYellow, (t - 0.5f) / 0.25f);
            return glm::mix(kYellow, kRed, (t - 0.75f) / 0.25f);
        }
    } // namespace OverdrawHeatmap
} // namespace OloEngine
