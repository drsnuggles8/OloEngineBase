#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace OloEngine
{
    // =========================================================================
    // Automatic exposure / eye adaptation — CPU mirror of the GLSL compute
    // shaders in:
    //   - assets/shaders/compute/AutoExposureHistogram.comp  (luminance -> bin)
    //   - assets/shaders/compute/AutoExposureAverage.comp     (histogram -> exposure)
    //
    // Both sides MUST match exactly so AutoExposureMathTest can pin the shader
    // path against this header (same contract as UnderwaterFog.h <-> the water
    // tone-map shader). If you change a formula here, change the .comp twin.
    //
    // Pipeline (per frame):
    //   1. Histogram pass bins every HDR pixel by log2(luminance) into 256 bins.
    //      Bin 0 is reserved for near-black pixels and excluded from the average.
    //   2. Average pass reduces the histogram to a weighted-average luminance
    //      (Alex Tardif / Bruno Opsenica "Automatic Exposure" histogram method),
    //      eases the *previous frame's* adapted luminance toward it (eye
    //      adaptation), then converts the adapted luminance to an exposure
    //      multiplier with the Lagarde "Moving Frostbite to PBR" EV100 model.
    //
    // References:
    //   - Alex Tardif, "Automatic Exposure Using a Luminance Histogram" (2019).
    //   - S. Lagarde, C. de Rousiers, "Moving Frostbite to Physically Based
    //     Rendering 3.0" (SIGGRAPH 2014), §5.1 (exposure / EV100).
    // =========================================================================
    namespace AutoExposure
    {
        // Number of histogram bins. MUST match local_size_x of the average
        // compute shader and the bin loop in the histogram shader.
        inline constexpr u32 kHistogramBins = 256u;

        // Rec.709 relative luminance weights — same as PostProcess_BloomThreshold.glsl.
        inline constexpr glm::vec3 kLuminanceWeights = glm::vec3(0.2126f, 0.7152f, 0.0722f);

        // Pixels below this luminance land in bin 0 and are excluded from the
        // average (prevents huge black regions from dragging exposure to the
        // brightest clamp). Mirrors EPSILON in the shaders.
        inline constexpr f32 kBlackThreshold = 0.0001f;

        // Relative luminance of an HDR colour (Rec.709).
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline f32 Luminance(const glm::vec3& hdrColor) noexcept
        {
            return glm::dot(glm::max(hdrColor, glm::vec3(0.0f)), kLuminanceWeights);
        }

        // Map a luminance value to a histogram bin in [0, 255].
        //   - bin 0      : near-black (lum < kBlackThreshold), excluded from the average.
        //   - bins 1..255: log2(lum) linearly mapped across [minLogLum, maxLogLum].
        // invLogLumRange == 1 / (maxLogLum - minLogLum).
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline u32 LuminanceToBin(f32 luminance, f32 minLogLum, f32 invLogLumRange) noexcept
        {
            if (!std::isfinite(luminance) || luminance < kBlackThreshold)
                return 0u;

            const f32 logLum = std::clamp((std::log2(luminance) - minLogLum) * invLogLumRange, 0.0f, 1.0f);
            // Bins 1..255 (bin 0 stays reserved for black). 254 buckets across [0,1].
            return static_cast<u32>(logLum * 254.0f + 1.0f);
        }

        // Inverse of LuminanceToBin for a (fractional) bin index in [1, 255].
        // Used to turn the weighted-average bin back into a luminance value.
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline f32 BinToLuminance(f32 bin, f32 minLogLum, f32 logLumRange) noexcept
        {
            const f32 logLum01 = std::clamp((bin - 1.0f) / 254.0f, 0.0f, 1.0f);
            return std::exp2(logLum01 * logLumRange + minLogLum);
        }

        // Reduce a 256-bin log-luminance histogram to an average linear
        // luminance. Bin 0 (black pixels) is excluded from the weighting; if the
        // whole frame is black the result floors at exp2(minLogLum).
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline f32 ComputeAverageLuminance(const std::array<u32, kHistogramBins>& histogram,
                                                                                                                           f32 minLogLum, f32 maxLogLum) noexcept
        {
            const f32 logLumRange = maxLogLum - minLogLum;
            if (!(logLumRange > 0.0f))
                return std::exp2(minLogLum);

            // weightedCount = sum(binIndex * count); bin 0 contributes nothing.
            u64 weightedCount = 0;
            u64 totalCount = 0;
            for (u32 i = 0; i < kHistogramBins; ++i)
            {
                weightedCount += static_cast<u64>(i) * histogram[i];
                totalCount += histogram[i];
            }

            const u64 numBlack = histogram[0];
            const u64 denom = (totalCount > numBlack) ? (totalCount - numBlack) : 1ULL;

            // Average bin index over the non-black pixels, in [0, 255].
            const f32 avgBin = static_cast<f32>(weightedCount) / static_cast<f32>(denom);
            return BinToLuminance(avgBin, minLogLum, logLumRange);
        }

        // Frame-rate-independent eye adaptation: ease `current` toward `target`.
        // Brightening (target > current) uses speedUp, darkening uses speedDown
        // (the eye dark-adapts slower than it light-adapts). With dt in seconds
        // the step is exact under subdivision: adapting dt then dt equals
        // adapting 2*dt in one shot.
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline f32 AdaptLuminance(f32 current, f32 target, f32 dt,
                                                                                                                  f32 speedUp, f32 speedDown) noexcept
        {
            if (!std::isfinite(current) || current <= 0.0f)
                current = target; // first frame / invalid history snaps to target
            if (!std::isfinite(target) || target < 0.0f)
                target = current;
            if (!std::isfinite(dt) || dt <= 0.0f)
                return current;

            const f32 speed = (target > current) ? speedUp : speedDown;
            if (!(speed > 0.0f))
                return target; // zero/invalid speed == instant adaptation

            const f32 factor = 1.0f - std::exp(-dt * speed);
            return current + (target - current) * factor;
        }

        // Lagarde/Frostbite EV100 from a scene luminance (ISO 100, K = 12.5).
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline f32 EV100FromLuminance(f32 luminance) noexcept
        {
            return std::log2(std::max(luminance, kBlackThreshold) * 100.0f / 12.5f);
        }

        // Sensor saturation-based exposure for a given EV100 (Frostbite eq.):
        //   maxLuminance = 1.2 * 2^EV100 ; exposure = 1 / maxLuminance.
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline f32 ExposureFromEV100(f32 ev100) noexcept
        {
            const f32 maxLuminance = 1.2f * std::exp2(ev100);
            return 1.0f / std::max(maxLuminance, kBlackThreshold);
        }

        // Full luminance -> exposure multiplier. `exposureCompensation` is in EV
        // stops: +1 EV doubles the resulting image brightness. The result is
        // clamped to [minExposure, maxExposure].
        [[nodiscard("pure computation; discarding the result makes the call a no-op")]] inline f32 ComputeExposure(f32 adaptedLuminance, f32 exposureCompensation,
                                                                                                                   f32 minExposure, f32 maxExposure) noexcept
        {
            const f32 ev100 = EV100FromLuminance(adaptedLuminance) - exposureCompensation;
            const f32 exposure = ExposureFromEV100(ev100);
            const f32 lo = std::min(minExposure, maxExposure);
            const f32 hi = std::max(minExposure, maxExposure);
            return std::clamp(exposure, lo, hi);
        }

        // The full per-frame step is just ComputeAverageLuminance -> AdaptLuminance
        // -> ComputeExposure; callers (the metering compute shader, the CPU tests)
        // chain those three directly so there's no wide-parameter wrapper here.
    } // namespace AutoExposure
} // namespace OloEngine
