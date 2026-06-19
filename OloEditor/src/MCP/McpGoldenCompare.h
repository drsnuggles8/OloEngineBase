#pragma once

// Pure, engine-light image-comparison core for the olo_render_compare_golden MCP
// tool (issue #316 Part 4). The MCP handler in McpTools.cpp captures the editor
// viewport (optionally from a fixed camera pose), decodes both the capture and a
// golden PNG into RGBA8 buffers, then hands them here for the numeric verdict.
//
// Keeping the diff math in a free function with NO GL / EnTT / renderer / editor /
// stb dependencies means it unit-tests directly (the test binary compiles this
// header but deliberately NOT McpTools.cpp), exactly like its siblings
// McpRenderExplain.h / McpPhysicsExplain.h.
//
// Consistency with the test-suite goldens (HANDOVER: "Don't reinvent"): the RMSE,
// SSIM and per-pixel diff math here MIRROR
// OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp so the MCP verdict
// agrees with the OLOENGINE_GOLDEN_REBASE workflow. The default (no caller
// threshold) verdict reproduces that file's cascaded RMSE -> SSIM decision with
// the same constants; an explicit caller threshold switches to a single,
// predictable SSIM gate the agent controls.
//
// Only OloEngine/Core/Base.h is pulled in (for the integer/float typedefs);
// everything else is the standard library.

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::MCP::GoldenCompare
{
    // The suite's pass/fail constants (GoldenImageTests.cpp §8). Re-stated here so
    // the MCP verdict matches the test-suite goldens bit-for-bit on the bounds.
    //   - RMSE <= kRmsePassBelow  -> surely a match (skip SSIM).
    //   - RMSE >= kRmseFailAbove  -> surely a regression (fail without SSIM).
    //   - in between              -> compute SSIM, pass iff >= kSsimPassThreshold.
    inline constexpr f32 kRmsePassBelow = 0.004f;
    inline constexpr f32 kRmseFailAbove = 0.02f;
    inline constexpr f32 kSsimPassThreshold = 0.985f;

    // Pixels whose worst channel delta exceeds this many 8-bit LSBs count as
    // "mismatched" in the reported MismatchPixels stat (suite's ComputeDiffStats).
    inline constexpr u32 kMismatchEpsilon = 4;

    struct CompareResult
    {
        // False (with everything else at defaults) when the captured frame and the
        // golden have different dimensions — the metrics are undefined, so the
        // tool reports the mismatch instead of a meaningless similarity number.
        bool DimensionsMatch = false;
        u32 ActualWidth = 0;
        u32 ActualHeight = 0;
        u32 GoldenWidth = 0;
        u32 GoldenHeight = 0;

        // RGB root-mean-square error, normalised to [0, 1]. 0 = identical.
        f32 Rmse = 1.0f;
        // Mean squared error over RGB, normalised to [0, 1] (Rmse == sqrt(Mse)).
        f32 Mse = 1.0f;
        // Mean SSIM over RGB in [0, 1]. 1 = identical, approaches 0 as structure
        // diverges. This is the headline Similarity the verdict gates on.
        f32 Ssim = 0.0f;
        // The headline [0, 1] similarity (== Ssim): 1.0 is a perfect match.
        f32 Similarity = 0.0f;

        // Per-pixel diagnostics (mirrors the suite's DiffStats), for pinpointing
        // WHERE the frame diverged when a compare fails.
        u32 MismatchPixels = 0; // pixels with any channel delta > kMismatchEpsilon
        u32 TotalPixels = 0;
        u32 MaxChannelDelta = 0; // worst single-channel abs delta across the frame
        u32 WorstX = 0;
        u32 WorstY = 0;

        // The pass/fail verdict and the threshold that produced it.
        bool Pass = false;
        f32 Threshold = kSsimPassThreshold; // effective SSIM pass threshold
        // "suite-cascade" (no caller threshold: full RMSE->SSIM cascade, suite
        // constants) or "explicit" (caller threshold: Similarity >= Threshold).
        std::string ThresholdMode = "suite-cascade";

        // Human-readable one-liner describing the verdict (RMSE/SSIM + worst pixel).
        std::string Message;
    };

    namespace Detail
    {
        // RMSE over RGB channels (alpha ignored), normalised to [0, 1]. Identical
        // to GoldenImageTests.cpp::ComputeRgbRmse. Returns 1.0 on size mismatch.
        [[nodiscard]] inline f64 ComputeRgbMse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1.0;
            const std::size_t pixelCount = a.size() / 4;
            f64 sumSq = 0.0;
            for (std::size_t i = 0; i < pixelCount; ++i)
            {
                for (u32 c = 0; c < 3; ++c) // RGB only
                {
                    const f64 d = (static_cast<f64>(a[i * 4 + c]) - static_cast<f64>(b[i * 4 + c])) / 255.0;
                    sumSq += d * d;
                }
            }
            return sumSq / static_cast<f64>(pixelCount * 3);
        }

        // Mean SSIM over RGB channels in [0, 1] on 8x8 non-overlapping windows.
        // Identical formulation to GoldenImageTests.cpp::ComputeRgbSsim (classic
        // Wang/Bovik 2004 constants, L = 255). Returns 1.0 for bit-identical input.
        [[nodiscard]] inline f32 ComputeRgbSsim(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height)
        {
            if (a.size() != b.size() || a.empty() || width == 0 || height == 0)
                return 0.0f;

            constexpr u32 kWindow = 8;
            constexpr f64 kC1 = (0.01 * 255.0) * (0.01 * 255.0);
            constexpr f64 kC2 = (0.03 * 255.0) * (0.03 * 255.0);

            const u32 winsX = width / kWindow;
            const u32 winsY = height / kWindow;
            if (winsX == 0 || winsY == 0)
            {
                // Frame smaller than one window — SSIM is ill-defined; fall back to
                // "very similar iff RMSE is tiny" (matches the suite).
                const f64 mse = ComputeRgbMse(a, b);
                return std::sqrt(mse) < 0.002 ? 1.0f : 0.0f;
            }

            f64 ssimSum = 0.0;
            u64 ssimCount = 0;
            for (u32 wy = 0; wy < winsY; ++wy)
            {
                for (u32 wx = 0; wx < winsX; ++wx)
                {
                    for (u32 ch = 0; ch < 3; ++ch)
                    {
                        f64 sumA = 0.0;
                        f64 sumB = 0.0;
                        for (u32 yy = 0; yy < kWindow; ++yy)
                        {
                            for (u32 xx = 0; xx < kWindow; ++xx)
                            {
                                const u32 x = wx * kWindow + xx;
                                const u32 y = wy * kWindow + yy;
                                const std::size_t idx = (static_cast<std::size_t>(y) * width + x) * 4 + ch;
                                sumA += static_cast<f64>(a[idx]);
                                sumB += static_cast<f64>(b[idx]);
                            }
                        }
                        constexpr f64 kN = static_cast<f64>(kWindow * kWindow);
                        const f64 meanA = sumA / kN;
                        const f64 meanB = sumB / kN;

                        f64 varA = 0.0;
                        f64 varB = 0.0;
                        f64 covAB = 0.0;
                        for (u32 yy = 0; yy < kWindow; ++yy)
                        {
                            for (u32 xx = 0; xx < kWindow; ++xx)
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
                        varA /= (kN - 1.0);
                        varB /= (kN - 1.0);
                        covAB /= (kN - 1.0);

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
    } // namespace Detail

    // Compare two decoded RGBA8 buffers (4 bytes/pixel, row-major) and produce the
    // numeric verdict. `threshold`, when set, is a minimum SSIM-similarity in
    // [0, 1] the caller controls (explicit mode); when unset, the verdict
    // reproduces the test-suite's cascaded RMSE -> SSIM decision (suite-cascade
    // mode). Dimension mismatch short-circuits to a non-passing
    // DimensionsMatch=false result.
    [[nodiscard]] inline CompareResult Compare(const std::vector<u8>& actual, u32 actualWidth, u32 actualHeight,
                                               const std::vector<u8>& golden, u32 goldenWidth, u32 goldenHeight,
                                               std::optional<f32> threshold)
    {
        CompareResult r;
        r.ActualWidth = actualWidth;
        r.ActualHeight = actualHeight;
        r.GoldenWidth = goldenWidth;
        r.GoldenHeight = goldenHeight;

        if (actualWidth != goldenWidth || actualHeight != goldenHeight)
        {
            r.DimensionsMatch = false;
            std::ostringstream msg;
            msg << "dimension mismatch: captured frame " << actualWidth << "x" << actualHeight
                << " vs golden " << goldenWidth << "x" << goldenHeight
                << " — the images must be the same size to compare. Set a deterministic capture size with "
                   "olo_viewport_set_size, or rebase the golden.";
            r.Message = msg.str();
            return r;
        }
        r.DimensionsMatch = true;
        r.TotalPixels = actualWidth * actualHeight;

        // Defensive: a mis-sized buffer (decode bug) would make the metrics
        // garbage and the diff loop read out of bounds. Treat as a hard fail.
        const std::size_t expectedBytes = static_cast<std::size_t>(actualWidth) * actualHeight * 4;
        if (actual.size() != expectedBytes || golden.size() != expectedBytes)
        {
            std::ostringstream msg;
            msg << "buffer size mismatch: expected " << expectedBytes << " RGBA bytes for "
                << actualWidth << "x" << actualHeight << ", got captured=" << actual.size()
                << " golden=" << golden.size();
            r.Message = msg.str();
            return r;
        }

        r.Mse = static_cast<f32>(Detail::ComputeRgbMse(actual, golden));
        r.Rmse = std::sqrt(r.Mse);
        r.Ssim = Detail::ComputeRgbSsim(actual, golden, actualWidth, actualHeight);
        r.Similarity = std::clamp(r.Ssim, 0.0f, 1.0f);

        // Per-pixel diagnostics (suite's ComputeDiffStats, sans the heatmap PNG —
        // the MCP tool returns numbers + the captured image, not a heatmap).
        for (u32 y = 0; y < actualHeight; ++y)
        {
            for (u32 x = 0; x < actualWidth; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * actualWidth + x) * 4;
                const u32 dr = static_cast<u32>(std::abs(static_cast<int>(actual[idx + 0]) - static_cast<int>(golden[idx + 0])));
                const u32 dg = static_cast<u32>(std::abs(static_cast<int>(actual[idx + 1]) - static_cast<int>(golden[idx + 1])));
                const u32 db = static_cast<u32>(std::abs(static_cast<int>(actual[idx + 2]) - static_cast<int>(golden[idx + 2])));
                const u32 dMax = std::max({ dr, dg, db });
                if (dMax > r.MaxChannelDelta)
                {
                    r.MaxChannelDelta = dMax;
                    r.WorstX = x;
                    r.WorstY = y;
                }
                if (dMax > kMismatchEpsilon)
                    ++r.MismatchPixels;
            }
        }

        // Verdict.
        std::ostringstream msg;
        if (threshold.has_value())
        {
            // Explicit mode: a single, predictable SSIM-similarity gate.
            r.ThresholdMode = "explicit";
            r.Threshold = std::clamp(*threshold, 0.0f, 1.0f);
            r.Pass = r.Similarity >= r.Threshold;
            msg << "SSIM similarity " << r.Similarity << (r.Pass ? " >= " : " < ") << r.Threshold
                << " (explicit threshold); RMSE " << r.Rmse;
        }
        else
        {
            // Suite-cascade mode: reproduce GoldenImageTests.cpp's verdict exactly.
            r.ThresholdMode = "suite-cascade";
            r.Threshold = kSsimPassThreshold;
            if (r.Rmse <= kRmsePassBelow)
            {
                r.Pass = true;
                msg << "RMSE " << r.Rmse << " <= " << kRmsePassBelow << " (fast pass); SSIM " << r.Ssim;
            }
            else if (r.Rmse >= kRmseFailAbove)
            {
                r.Pass = false;
                msg << "RMSE " << r.Rmse << " >= " << kRmseFailAbove << " (hard fail); SSIM " << r.Ssim;
            }
            else
            {
                r.Pass = r.Ssim >= kSsimPassThreshold;
                msg << "RMSE " << r.Rmse << " in ambiguous band (" << kRmsePassBelow << ".." << kRmseFailAbove
                    << "); SSIM " << r.Ssim << (r.Pass ? " >= " : " < ") << kSsimPassThreshold;
            }
        }
        if (r.MaxChannelDelta > 0)
        {
            msg << ". Worst pixel (" << r.WorstX << "," << r.WorstY << ") max channel delta " << r.MaxChannelDelta
                << "; " << r.MismatchPixels << "/" << r.TotalPixels << " pixels differ by > " << kMismatchEpsilon
                << " LSB.";
        }
        else
        {
            msg << ". Bit-identical to the golden.";
        }
        r.Message = msg.str();
        return r;
    }
} // namespace OloEngine::MCP::GoldenCompare
