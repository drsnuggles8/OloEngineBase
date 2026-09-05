#include "OloEnginePCH.h"
#include "VisualEvidenceGuards.h"

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
} // namespace OloEngine::Tests::VisualEvidence
