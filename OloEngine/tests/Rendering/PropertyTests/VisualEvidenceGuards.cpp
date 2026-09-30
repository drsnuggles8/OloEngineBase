#include "OloEnginePCH.h"
#include "VisualEvidenceGuards.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace OloEngine::Tests::VisualEvidence
{
    f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
    {
        if (a.size() != b.size() || a.empty())
        {
            return std::numeric_limits<f64>::max();
        }

        f64 sumSq = 0.0;
        sizet count = 0;
        for (sizet i = 0; i + 3 < a.size(); i += 4)
        {
            for (sizet c = 0; c < 3; ++c)
            {
                const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                sumSq += d * d;
                ++count;
            }
        }
        return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
    }

    void FlipRgbaRowsInPlace(std::vector<u8>& pixels, u32 width, u32 height)
    {
        const sizet rowBytes = static_cast<sizet>(width) * 4u;
        if (height < 2u || pixels.size() != rowBytes * height)
        {
            return;
        }

        std::vector<u8> scratch(rowBytes);
        for (u32 y = 0; y < height / 2u; ++y)
        {
            u8* top = pixels.data() + static_cast<sizet>(y) * rowBytes;
            u8* bottom = pixels.data() + static_cast<sizet>(height - 1u - y) * rowBytes;
            std::memcpy(scratch.data(), top, rowBytes);
            std::memcpy(top, bottom, rowBytes);
            std::memcpy(bottom, scratch.data(), rowBytes);
        }
    }

    f64 SubjectCoverage(const std::vector<u8>& pixels, const std::function<bool(u32, u32, u32)>& isSubject)
    {
        if (pixels.size() < 4u || !isSubject)
        {
            return 0.0;
        }

        sizet hits = 0;
        sizet total = 0;
        for (sizet i = 0; i + 3 < pixels.size(); i += 4)
        {
            ++total;
            if (isSubject(pixels[i + 0], pixels[i + 1], pixels[i + 2]))
            {
                ++hits;
            }
        }
        return total ? static_cast<f64>(hits) / static_cast<f64>(total) : 0.0;
    }

    void ExpectFrameHasSubject(const std::vector<u8>& pixels, const std::string& poseName,
                               const std::function<bool(u32, u32, u32)>& isSubject, f64 minCoverage)
    {
        const f64 coverage = SubjectCoverage(pixels, isSubject);
        EXPECT_GE(coverage, minCoverage)
            << "pose '" << poseName << "': the content mask found " << (coverage * 100.0)
            << "% of the frame, below the " << (minCoverage * 100.0)
            << "% floor — the subject is not on screen. A frame can be a perfectly "
               "valid sky and still prove nothing (issue #931).";
    }

    void ExpectCapturesAreDistinct(const std::vector<std::vector<u8>>& captures,
                                   const std::vector<std::string>& poseNames,
                                   f64 noiseFloorRmse, f64 margin)
    {
        ASSERT_EQ(captures.size(), poseNames.size()) << "one name per capture";
        ASSERT_GE(captures.size(), 2u) << "distinctness needs at least two captures";

        // A perfectly deterministic renderer measures a floor of 0, which would
        // make any threshold derived from it 0 as well — and "differs from an
        // identical frame by more than nothing" is not a check. Floor the floor.
        constexpr f64 kMinimumFloor = 0.05;
        const f64 threshold = std::max(noiseFloorRmse, kMinimumFloor) * margin;

        for (sizet i = 0; i < captures.size(); ++i)
        {
            for (sizet j = i + 1; j < captures.size(); ++j)
            {
                const f64 rmse = Rgba8Rmse(captures[i], captures[j]);
                EXPECT_GT(rmse, threshold)
                    << "poses '" << poseNames[i] << "' and '" << poseNames[j] << "' differ by RMSE "
                    << rmse << ", at or below " << margin << "x the measured noise floor of "
                    << noiseFloorRmse << ". Two different camera poses rendered the same picture — "
                                         "the camera did not move (issue #931).";
            }
        }
    }
    f32 Rgba8Ssim(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height)
    {
        if (a.size() != b.size() || (a.size() % 4 != 0 || a.size() / 4 != static_cast<u64>(width) * height) || a.empty() || width == 0 || height == 0)
            return 0.0f;

        constexpr u32 kWindow = 8;
        constexpr f64 kC1 = (0.01 * 255.0) * (0.01 * 255.0);
        constexpr f64 kC2 = (0.03 * 255.0) * (0.03 * 255.0);

        const u32 winsX = width / kWindow + (width % kWindow != 0u ? 1u : 0u);
        const u32 winsY = height / kWindow + (height % kWindow != 0u ? 1u : 0u);

        f64 ssimSum = 0.0;
        u64 ssimCount = 0;

        for (u32 wy = 0; wy < winsY; ++wy)
        {
            for (u32 wx = 0; wx < winsX; ++wx)
            {
                const u32 windowWidth = std::min(kWindow, width - wx * kWindow);
                const u32 windowHeight = std::min(kWindow, height - wy * kWindow);
                const f64 sampleCount = static_cast<f64>(windowWidth * windowHeight);
                // A singleton edge window has zero variance/covariance.
                const f64 varianceDivisor = std::max(sampleCount - 1.0, 1.0);
                for (u32 ch = 0; ch < 3; ++ch)
                {
                    // Two passes per window: mean, then variance + covariance.
                    f64 sumA = 0.0, sumB = 0.0;
                    for (u32 yy = 0; yy < windowHeight; ++yy)
                    {
                        for (u32 xx = 0; xx < windowWidth; ++xx)
                        {
                            const u32 x = wx * kWindow + xx;
                            const u32 y = wy * kWindow + yy;
                            const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4 + ch;
                            sumA += static_cast<f64>(a[idx]);
                            sumB += static_cast<f64>(b[idx]);
                        }
                    }
                    const f64 meanA = sumA / sampleCount;
                    const f64 meanB = sumB / sampleCount;

                    f64 varA = 0.0, varB = 0.0, covAB = 0.0;
                    for (u32 yy = 0; yy < windowHeight; ++yy)
                    {
                        for (u32 xx = 0; xx < windowWidth; ++xx)
                        {
                            const u32 x = wx * kWindow + xx;
                            const u32 y = wy * kWindow + yy;
                            const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4 + ch;
                            const f64 da = static_cast<f64>(a[idx]) - meanA;
                            const f64 db = static_cast<f64>(b[idx]) - meanB;
                            varA += da * da;
                            varB += db * db;
                            covAB += da * db;
                        }
                    }
                    varA /= varianceDivisor;
                    varB /= varianceDivisor;
                    covAB /= varianceDivisor;

                    const f64 numerator = (2.0 * meanA * meanB + kC1) * (2.0 * covAB + kC2);
                    const f64 denominator = (meanA * meanA + meanB * meanB + kC1) * (varA + varB + kC2);
                    const f64 ssim = denominator > 0.0 ? (numerator / denominator) : 1.0;
                    ssimSum += ssim;
                    ++ssimCount;
                }
            }
        }

        return static_cast<f32>(ssimSum / static_cast<f64>(ssimCount));
    }

    namespace
    {
        [[nodiscard]] f32 Rec601Luma(const std::vector<u8>& rgba, sizet pixelIndex)
        {
            const sizet i = pixelIndex * 4u;
            return 0.299f * static_cast<f32>(rgba[i + 0]) + 0.587f * static_cast<f32>(rgba[i + 1]) +
                   0.114f * static_cast<f32>(rgba[i + 2]);
        }
    } // namespace

    f64 FineDetailDensity(const std::vector<u8>& rgba, u32 width, u32 height, const PixelRect& crop, f32 threshold)
    {
        constexpr f64 kInvalid = std::numeric_limits<f64>::quiet_NaN();
        if (width == 0u || height == 0u || crop.Width == 0u || crop.Height == 0u)
            return kInvalid;
        if (rgba.size() != static_cast<sizet>(width) * height * 4u)
            return kInvalid;
        if (!std::isfinite(threshold) || threshold < 0.0f)
            return kInvalid;
        // 64-bit sums so a hostile crop cannot wrap into range.
        if (static_cast<u64>(crop.X) + crop.Width > width || static_cast<u64>(crop.Y) + crop.Height > height)
            return kInvalid;

        const auto lumaAt = [&](u32 x, u32 y)
        { return Rec601Luma(rgba, static_cast<sizet>(y) * width + x); };

        sizet above = 0;
        for (u32 y = crop.Y; y < crop.Y + crop.Height; ++y)
        {
            const u32 yNext = y + 1u < height ? y + 1u : y;
            for (u32 x = crop.X; x < crop.X + crop.Width; ++x)
            {
                const u32 xNext = x + 1u < width ? x + 1u : x;
                const f32 gx = lumaAt(xNext, y) - lumaAt(x, y);
                const f32 gy = lumaAt(x, yNext) - lumaAt(x, y);
                if (std::sqrt(gx * gx + gy * gy) > threshold)
                    ++above;
            }
        }
        return static_cast<f64>(above) / (static_cast<f64>(crop.Width) * static_cast<f64>(crop.Height));
    }

    f64 FineDetailDensity(const std::vector<u8>& rgba, u32 width, u32 height, f32 threshold)
    {
        return FineDetailDensity(rgba, width, height, PixelRect{ 0u, 0u, width, height }, threshold);
    }

    f64 ExpectFineDetailNotReduced(const std::vector<u8>& featureOn, const std::vector<u8>& featureOff, u32 width,
                                   u32 height, const PixelRect& crop, const std::string& label, f64 slack)
    {
        const f64 on = FineDetailDensity(featureOn, width, height, crop);
        const f64 off = FineDetailDensity(featureOff, width, height, crop);
        if (!std::isfinite(on) || !std::isfinite(off))
        {
            ADD_FAILURE() << label << ": fine-detail density is not measurable (on=" << on << ", off=" << off
                          << "); a buffer is not " << width << "x" << height
                          << " RGBA8, or the crop leaves the frame. The A/B measured nothing.";
            return std::numeric_limits<f64>::quiet_NaN();
        }

        EXPECT_GE(on + slack, off)
            << label << ": turning the feature ON REDUCED fine-detail density from " << off * 100.0 << "% to "
            << on * 100.0 << "% of pixels above the gradient threshold (slack " << slack * 100.0
            << "%). A feature that claims to add geometric richness must not remove detail. Enlarge the two captures "
               "3x and describe what is on screen before touching this bound (issue #1401).";

        return off > 0.0 ? on / off : std::numeric_limits<f64>::quiet_NaN();
    }

    f64 ExpectFineDetailFloorAgainstReference(const std::vector<u8>& frame, u32 width, u32 height, const PixelRect& crop,
                                              const std::vector<u8>& reference, u32 referenceWidth,
                                              u32 referenceHeight, f64 minFraction, const std::string& label)
    {
        const f64 measured = FineDetailDensity(frame, width, height, crop);
        const f64 ref = FineDetailDensity(reference, referenceWidth, referenceHeight);
        if (!std::isfinite(measured) || !std::isfinite(ref) || ref <= 0.0 || !std::isfinite(minFraction))
        {
            ADD_FAILURE() << label << ": the reference floor is not measurable (frame=" << measured
                          << ", reference=" << ref << ", minFraction=" << minFraction
                          << "). The floor measured nothing.";
            return std::numeric_limits<f64>::quiet_NaN();
        }

        EXPECT_GE(measured, ref * minFraction)
            << label << ": fine-detail density " << measured * 100.0 << "% of pixels is below " << minFraction
            << "x the reference photograph's " << ref * 100.0
            << "%. The subject is far flatter than real foliage (issue #1401). This is the WEAKER check: look at "
               "the frame, enlarged 3x, before adjusting anything.";
        return measured / ref;
    }
} // namespace OloEngine::Tests::VisualEvidence
