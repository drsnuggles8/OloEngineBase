#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomCoverage.h"

#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace OloEngine::GroomCoverage
{
    namespace
    {
        // A strand narrower than this many pixels is widened to it and pays for
        // the widening in alpha. One pixel is not a tuning constant: it is the
        // width below which a rasteriser stops producing a fragment per pixel
        // along the strand, which is the discontinuity the widening exists to
        // remove. Expressed as a HALF width because every interior computation
        // is in half widths.
        constexpr f32 kMinHalfWidthPixels = 0.5f;

        // Guards the reference rasteriser against an allocation sized by a
        // caller's arithmetic rather than by intent. 16 * 16 samples over a
        // 1920x1080 frame is 530M samples = 66 MB of bits, which is the
        // largest configuration the analysis uses.
        constexpr u64 kMaxReferenceSamples = 1ull << 30;

        // A segment's screen footprint: a TAPERED BAND with SQUARE ends.
        //
        // Not a round-capped capsule, and the difference is not cosmetic.
        // GroomStrandMesh emits one QUAD per segment and the vertex shader
        // widens it sideways, so the drawn shape has no end caps at all.
        // Modelling it with caps adds a half-disc of the RASTERISED half width
        // at each end — and because that half width has a half-pixel floor,
        // each cap is about 0.79 px^2 however thin the strand really is. On a
        // 17 500-segment sub-pixel case that spurious area measured roughly
        // +20 % of the entire silhouette, i.e. the model was reporting
        // coverage for a shape the GPU never draws. Square ends, because
        // square ends are what ships.
        struct TaperedBand
        {
            glm::vec2 A{ 0.0f };
            glm::vec2 AB{ 0.0f };
            f32 InvLengthSquared = 0.0f;
            f32 HalfWidthA = 0.0f;
            f32 HalfWidthDelta = 0.0f;
            bool Degenerate = false;
        };

        [[nodiscard]] TaperedBand MakeBand(const glm::vec2& a, const glm::vec2& b, f32 halfWidthA,
                                           f32 halfWidthB) noexcept
        {
            TaperedBand band;
            band.A = a;
            band.AB = b - a;
            const f32 lengthSquared = glm::dot(band.AB, band.AB);
            // Two coincident control points are legal in a cooked groom and
            // have no direction to widen along; the quad emitted for them
            // collapses, so the honest model is a dot of the rasterised half
            // width rather than nothing at all.
            band.Degenerate = !(lengthSquared > 0.0f);
            band.InvLengthSquared = band.Degenerate ? 0.0f : 1.0f / lengthSquared;
            band.HalfWidthA = halfWidthA;
            band.HalfWidthDelta = halfWidthB - halfWidthA;
            return band;
        }

        // Returns the interpolation parameter along the band, or a negative
        // value when the sample is outside it. Written to return `t` rather
        // than a bool because every caller needs the half width there too, and
        // recomputing it was the obvious way for the coverage test and the
        // alpha to end up disagreeing.
        [[nodiscard]] f32 BandHit(const TaperedBand& band, const glm::vec2& sample) noexcept
        {
            const glm::vec2 toSample = sample - band.A;
            if (band.Degenerate)
            {
                return glm::dot(toSample, toSample) <= band.HalfWidthA * band.HalfWidthA ? 0.0f : -1.0f;
            }

            const f32 t = glm::dot(toSample, band.AB) * band.InvLengthSquared;
            // The square ends: a sample past either end of the segment is
            // outside, rather than being pulled back onto a cap that is not
            // there.
            if (t < 0.0f || t > 1.0f)
            {
                return -1.0f;
            }
            const glm::vec2 offset = toSample - band.AB * t;
            const f32 halfWidth = band.HalfWidthA + band.HalfWidthDelta * t;
            return glm::dot(offset, offset) <= halfWidth * halfWidth ? t : -1.0f;
        }

        struct PixelBounds
        {
            i32 MinX = 0;
            i32 MinY = 0;
            i32 MaxX = -1; // inclusive; MaxX < MinX means "nothing"
            i32 MaxY = -1;
        };

        [[nodiscard]] PixelBounds SegmentPixelBounds(const glm::vec2& a, const glm::vec2& b, f32 maxHalfWidth, u32 width,
                                                     u32 height) noexcept
        {
            PixelBounds bounds;
            const f32 minX = std::min(a.x, b.x) - maxHalfWidth;
            const f32 maxX = std::max(a.x, b.x) + maxHalfWidth;
            const f32 minY = std::min(a.y, b.y) - maxHalfWidth;
            const f32 maxY = std::max(a.y, b.y) + maxHalfWidth;

            bounds.MinX = std::max(0, static_cast<i32>(std::floor(minX)));
            bounds.MinY = std::max(0, static_cast<i32>(std::floor(minY)));
            bounds.MaxX = std::min(static_cast<i32>(width) - 1, static_cast<i32>(std::ceil(maxX)));
            bounds.MaxY = std::min(static_cast<i32>(height) - 1, static_cast<i32>(std::ceil(maxY)));
            return bounds;
        }

        // The alpha a fragment carries at parameter `t`: the ratio of the true
        // strand width to the width it was widened to. See the header — this
        // is the whole of sub-pixel coverage.
        [[nodiscard]] f32 WidenedAlpha(f32 trueHalfWidth) noexcept
        {
            if (trueHalfWidth >= kMinHalfWidthPixels)
            {
                return 1.0f;
            }
            return std::clamp(trueHalfWidth / kMinHalfWidthPixels, 0.0f, 1.0f);
        }

        [[nodiscard]] bool IsFinite(const glm::vec3& v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // One frame of one mode. Shared by ModeCoverage (which averages several
        // of these) and MeasureTemporalStability (which diffs them), so the two
        // cannot disagree about what a frame is.
        [[nodiscard]] std::vector<f32> SingleFrameCoverage(const std::vector<ScreenSegment>& segments, u32 width,
                                                           u32 height, GroomCompositionMode mode,
                                                           const ModeParameters& parameters, u32 frameIndex)
        {
            const sizet pixelCount = static_cast<sizet>(width) * static_cast<sizet>(height);
            std::vector<f32> coverage(pixelCount, 0.0f);

            // Per-mode accumulator. Only one of these is ever used; they are
            // declared together so the segment loop below has a single shape.
            std::vector<u8> binary;
            std::vector<u32> sampleMask;
            std::vector<f32> transmittance;

            switch (mode)
            {
                case GroomCompositionMode::OpaqueRibbon:
                case GroomCompositionMode::StochasticAlpha:
                    binary.assign(pixelCount, 0u);
                    break;
                case GroomCompositionMode::AlphaToCoverage:
                    sampleMask.assign(pixelCount, 0u);
                    break;
                case GroomCompositionMode::WeightedBlendedOIT:
                    transmittance.assign(pixelCount, 1.0f);
                    break;
                case GroomCompositionMode::Count:
                    return {};
            }

            for (const auto& segment : segments)
            {
                // The WIDENED band is what rasterises; the true half widths
                // stay available through WidenedAlpha for the fragment's alpha.
                const f32 widenedA = std::max(segment.HalfWidthA, kMinHalfWidthPixels);
                const f32 widenedB = std::max(segment.HalfWidthB, kMinHalfWidthPixels);
                const TaperedBand band = MakeBand(segment.A, segment.B, widenedA, widenedB);
                const PixelBounds bounds =
                    SegmentPixelBounds(segment.A, segment.B, std::max(widenedA, widenedB), width, height);

                for (i32 y = bounds.MinY; y <= bounds.MaxY; ++y)
                {
                    for (i32 x = bounds.MinX; x <= bounds.MaxX; ++x)
                    {
                        const glm::vec2 centre{ static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f };
                        const f32 t = BandHit(band, centre);
                        if (t < 0.0f)
                        {
                            continue;
                        }

                        const f32 trueHalfWidth =
                            segment.HalfWidthA + (segment.HalfWidthB - segment.HalfWidthA) * t;
                        const f32 alpha = WidenedAlpha(trueHalfWidth);
                        const sizet index = static_cast<sizet>(y) * static_cast<sizet>(width) + static_cast<sizet>(x);

                        switch (mode)
                        {
                            case GroomCompositionMode::OpaqueRibbon:
                                if (alpha >= parameters.AlphaCutoff)
                                {
                                    binary[index] = 1u;
                                }
                                break;

                            case GroomCompositionMode::StochasticAlpha:
                            {
                                const f32 threshold = StochasticHash(static_cast<u32>(x), static_cast<u32>(y),
                                                                     frameIndex, segment.Id, parameters.StochasticSeed);
                                if (threshold < alpha)
                                {
                                    binary[index] = 1u;
                                }
                                break;
                            }

                            case GroomCompositionMode::AlphaToCoverage:
                                sampleMask[index] |= AlphaToCoverageMask(alpha, parameters.SampleCount);
                                break;

                            case GroomCompositionMode::WeightedBlendedOIT:
                                transmittance[index] *= (1.0f - alpha);
                                break;

                            case GroomCompositionMode::Count:
                                break;
                        }
                    }
                }
            }

            switch (mode)
            {
                case GroomCompositionMode::OpaqueRibbon:
                case GroomCompositionMode::StochasticAlpha:
                    for (sizet i = 0; i < pixelCount; ++i)
                    {
                        coverage[i] = binary[i] != 0u ? 1.0f : 0.0f;
                    }
                    break;

                case GroomCompositionMode::AlphaToCoverage:
                {
                    const f32 invSamples = 1.0f / static_cast<f32>(parameters.SampleCount);
                    for (sizet i = 0; i < pixelCount; ++i)
                    {
                        coverage[i] = static_cast<f32>(std::popcount(sampleMask[i])) * invSamples;
                    }
                    break;
                }

                case GroomCompositionMode::WeightedBlendedOIT:
                    for (sizet i = 0; i < pixelCount; ++i)
                    {
                        coverage[i] = 1.0f - transmittance[i];
                    }
                    break;

                case GroomCompositionMode::Count:
                    break;
            }

            return coverage;
        }

        [[nodiscard]] bool ParametersAreValid(GroomCompositionMode mode, const ModeParameters& parameters) noexcept
        {
            if (!IsValidGroomCompositionMode(static_cast<i32>(mode)))
            {
                return false;
            }
            if (!std::isfinite(parameters.AlphaCutoff) || parameters.AlphaCutoff < 0.0f || parameters.AlphaCutoff > 1.0f)
            {
                return false;
            }
            if (parameters.TemporalFrames == 0u)
            {
                return false;
            }
            // Only the mode that reads SampleCount is allowed to reject on it,
            // so a caller measuring OpaqueRibbon is not forced to supply a
            // meaningful sample count.
            if (mode == GroomCompositionMode::AlphaToCoverage)
            {
                if (parameters.SampleCount != 2u && parameters.SampleCount != 4u && parameters.SampleCount != 8u)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    // -------------------------------------------------------------------------

    f32 StochasticHash(u32 pixelX, u32 pixelY, u32 frameIndex, u32 segmentId, u32 seed) noexcept
    {
        // Mixing constants are the usual large odd primes; the finaliser is
        // the `lowbias32` avalanche. Every operation here is a 32-bit unsigned
        // one with defined wraparound in both C++ and GLSL, which is what lets
        // GroomStrand.glsl compute the identical value — see the header, and
        // GroomStrandGpuParityTest, which fails if either side drifts.
        u32 h = pixelX * 73856093u;
        h ^= pixelY * 19349663u;
        h ^= segmentId * 83492791u;
        h ^= frameIndex * 2654435761u;
        h ^= seed * 40503u;

        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;

        // 24 bits into a float's exact-integer range: the result is in [0,1)
        // with a uniform 2^-24 grid and can never round to exactly 1.0, which
        // a `h * 2^-32` formulation can.
        return static_cast<f32>(h >> 8) * (1.0f / 16777216.0f);
    }

    u32 AlphaToCoverageMask(f32 alpha, u32 sampleCount) noexcept
    {
        if (sampleCount == 0u || sampleCount > 32u || !std::isfinite(alpha))
        {
            return 0u;
        }
        const f32 clamped = std::clamp(alpha, 0.0f, 1.0f);
        const u32 bits = static_cast<u32>(std::lround(clamped * static_cast<f32>(sampleCount)));
        const u32 taken = std::min(bits, sampleCount);
        if (taken == 0u)
        {
            return 0u;
        }
        if (taken >= 32u)
        {
            return 0xFFFFFFFFu;
        }
        return (1u << taken) - 1u;
    }

    // -------------------------------------------------------------------------

    ProjectionStats ProjectGroom(const GroomAsset& groom, const glm::mat4& model, const glm::mat4& view,
                                 const glm::mat4& projection, u32 viewportWidth, u32 viewportHeight, f32 widthScale,
                                 std::vector<ScreenSegment>& outSegments)
    {
        outSegments.clear();
        ProjectionStats stats;

        if (viewportWidth == 0u || viewportHeight == 0u || !std::isfinite(widthScale) || widthScale < 0.0f)
        {
            return stats;
        }

        // Mean axis length: the same scalar AlembicGroomImporter uses to scale
        // widths through a non-uniform transform. One number cannot describe an
        // anisotropically scaled strand, and pretending otherwise here would
        // disagree with the cooked widths it is being applied to.
        const f32 axisX = glm::length(glm::vec3(model[0]));
        const f32 axisY = glm::length(glm::vec3(model[1]));
        const f32 axisZ = glm::length(glm::vec3(model[2]));
        const f32 objectScale = (axisX + axisY + axisZ) / 3.0f;

        const glm::mat4 modelView = view * model;
        const glm::mat4 mvp = projection * modelView;

        // Pixels per world unit at clip-w 1. Exact for a perspective
        // projection (projection[1][1] == 1/tan(fovY/2)) and for an
        // orthographic one (where clip w is 1 and projection[1][1] is
        // 2/orthoHeight), so no branch on projection type is needed.
        const f32 pixelsPerUnitAtUnitW =
            projection[1][1] * static_cast<f32>(viewportHeight) * 0.5f;

        const auto& points = groom.GetPoints();
        const auto& widths = groom.GetPointWidths();
        const u32 curveCount = groom.GetCurveCount();

        f32 minHalf = std::numeric_limits<f32>::max();
        f32 maxHalf = 0.0f;
        f64 sumHalf = 0.0;

        struct Projected
        {
            glm::vec2 Screen{ 0.0f };
            f32 HalfWidth = 0.0f;
            f32 Depth = 0.0f;
            bool Valid = false;
        };

        const auto project = [&](u32 pointIndex) -> Projected {
            Projected result;
            const glm::vec3& p = points[pointIndex];
            if (!IsFinite(p))
            {
                return result;
            }
            const glm::vec4 clip = mvp * glm::vec4(p, 1.0f);
            if (!(clip.w > 0.0f) || !std::isfinite(clip.w) || !std::isfinite(clip.x) || !std::isfinite(clip.y)
                || !std::isfinite(clip.z))
            {
                return result;
            }
            const f32 invW = 1.0f / clip.w;
            const f32 ndcX = clip.x * invW;
            const f32 ndcY = clip.y * invW;
            const f32 ndcZ = clip.z * invW;

            result.Screen.x = (ndcX * 0.5f + 0.5f) * static_cast<f32>(viewportWidth);
            // Y is flipped so row 0 is the top of the image, matching the
            // layout every consumer of these buffers uses (PNG rows, the
            // readback in the visual-evidence tests).
            result.Screen.y = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<f32>(viewportHeight);
            result.Depth = ndcZ * 0.5f + 0.5f;

            const f32 diameter = widths[pointIndex];
            const f32 radiusWorld = diameter * 0.5f * widthScale * objectScale;
            result.HalfWidth = radiusWorld * pixelsPerUnitAtUnitW * invW;
            result.Valid = std::isfinite(result.Screen.x) && std::isfinite(result.Screen.y)
                           && std::isfinite(result.HalfWidth) && result.HalfWidth >= 0.0f;
            return result;
        };

        for (u32 curve = 0; curve < curveCount; ++curve)
        {
            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 count = groom.GetCurvePointCount(curve);
            if (count < 2u)
            {
                continue;
            }

            Projected previous = project(first);
            for (u32 i = 1; i < count; ++i)
            {
                const Projected current = project(first + i);

                if (!previous.Valid || !current.Valid)
                {
                    // Distinguish "behind the camera" from "arithmetically
                    // broken": the first is ordinary framing, the second is a
                    // groom or a matrix worth a second look, and one counter
                    // for both would make the ordinary case hide the other.
                    const glm::vec3& pa = points[first + i - 1u];
                    const glm::vec3& pb = points[first + i];
                    if (!IsFinite(pa) || !IsFinite(pb))
                    {
                        ++stats.SegmentsDroppedNonFinite;
                    }
                    else
                    {
                        ++stats.SegmentsDroppedBehindCamera;
                    }
                    previous = current;
                    continue;
                }

                ScreenSegment segment;
                segment.A = previous.Screen;
                segment.B = current.Screen;
                segment.HalfWidthA = previous.HalfWidth;
                segment.HalfWidthB = current.HalfWidth;
                segment.DepthA = previous.Depth;
                segment.DepthB = current.Depth;
                // Curve index in the high bits, segment index in the low ones:
                // a pure function of the groom, stable across runs, and
                // distinct for two segments of the same strand so the
                // stochastic hash decorrelates along a strand as well as
                // between strands.
                segment.Id = GroomSegmentIdentity(curve, i);
                outSegments.push_back(segment);

                ++stats.SegmentsProjected;
                minHalf = std::min(minHalf, std::min(segment.HalfWidthA, segment.HalfWidthB));
                maxHalf = std::max(maxHalf, std::max(segment.HalfWidthA, segment.HalfWidthB));
                sumHalf += 0.5 * (static_cast<f64>(segment.HalfWidthA) + static_cast<f64>(segment.HalfWidthB));

                previous = current;
            }
        }

        if (stats.SegmentsProjected > 0u)
        {
            stats.MinHalfWidthPixels = minHalf;
            stats.MaxHalfWidthPixels = maxHalf;
            stats.MeanHalfWidthPixels = static_cast<f32>(sumHalf / static_cast<f64>(stats.SegmentsProjected));
        }
        return stats;
    }

    // -------------------------------------------------------------------------

    std::vector<f32> ReferenceCoverage(const std::vector<ScreenSegment>& segments, u32 width, u32 height,
                                       u32 supersample)
    {
        if (width == 0u || height == 0u || supersample < 4u)
        {
            return {};
        }

        const u64 sampleCount = static_cast<u64>(width) * static_cast<u64>(height) * static_cast<u64>(supersample)
                                * static_cast<u64>(supersample);
        if (sampleCount > kMaxReferenceSamples)
        {
            return {};
        }

        const u32 sampleWidth = width * supersample;
        // One bit per sub-sample. The reference is a UNION, not a sum, so the
        // hits have to be remembered rather than counted as they arrive —
        // counting would make two strands overlapping the same sub-sample read
        // as two hundred percent of a pixel.
        std::vector<u64> hits(static_cast<sizet>((sampleCount + 63ull) / 64ull), 0ull);

        const f32 step = 1.0f / static_cast<f32>(supersample);
        const f32 halfStep = step * 0.5f;

        for (const auto& segment : segments)
        {
            const TaperedBand band = MakeBand(segment.A, segment.B, segment.HalfWidthA, segment.HalfWidthB);
            const f32 maxHalfWidth = std::max(segment.HalfWidthA, segment.HalfWidthB);
            const PixelBounds bounds = SegmentPixelBounds(segment.A, segment.B, maxHalfWidth, width, height);

            for (i32 y = bounds.MinY; y <= bounds.MaxY; ++y)
            {
                for (i32 x = bounds.MinX; x <= bounds.MaxX; ++x)
                {
                    for (u32 sy = 0; sy < supersample; ++sy)
                    {
                        const f32 sampleY = static_cast<f32>(y) + halfStep + step * static_cast<f32>(sy);
                        for (u32 sx = 0; sx < supersample; ++sx)
                        {
                            const f32 sampleX = static_cast<f32>(x) + halfStep + step * static_cast<f32>(sx);
                            if (BandHit(band, glm::vec2{ sampleX, sampleY }) < 0.0f)
                            {
                                continue;
                            }
                            const u64 index = (static_cast<u64>(y) * static_cast<u64>(supersample) + sy)
                                                  * static_cast<u64>(sampleWidth)
                                              + static_cast<u64>(x) * static_cast<u64>(supersample) + sx;
                            hits[static_cast<sizet>(index >> 6)] |= 1ull << (index & 63ull);
                        }
                    }
                }
            }
        }

        std::vector<f32> coverage(static_cast<sizet>(width) * static_cast<sizet>(height), 0.0f);
        const f32 invSamplesPerPixel = 1.0f / static_cast<f32>(supersample * supersample);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                u32 covered = 0;
                for (u32 sy = 0; sy < supersample; ++sy)
                {
                    const u64 rowBase = (static_cast<u64>(y) * static_cast<u64>(supersample) + sy)
                                            * static_cast<u64>(sampleWidth)
                                        + static_cast<u64>(x) * static_cast<u64>(supersample);
                    for (u32 sx = 0; sx < supersample; ++sx)
                    {
                        const u64 index = rowBase + sx;
                        covered += (hits[static_cast<sizet>(index >> 6)] >> (index & 63ull)) & 1ull ? 1u : 0u;
                    }
                }
                coverage[static_cast<sizet>(y) * static_cast<sizet>(width) + x] =
                    static_cast<f32>(covered) * invSamplesPerPixel;
            }
        }
        return coverage;
    }

    // -------------------------------------------------------------------------

    std::vector<f32> ModeCoverage(const std::vector<ScreenSegment>& segments, u32 width, u32 height,
                                  GroomCompositionMode mode, const ModeParameters& parameters)
    {
        if (width == 0u || height == 0u || !ParametersAreValid(mode, parameters))
        {
            return {};
        }

        if (parameters.TemporalFrames == 1u)
        {
            return SingleFrameCoverage(segments, width, height, mode, parameters, 0u);
        }

        const sizet pixelCount = static_cast<sizet>(width) * static_cast<sizet>(height);
        std::vector<f64> accumulated(pixelCount, 0.0);
        for (u32 frame = 0; frame < parameters.TemporalFrames; ++frame)
        {
            const std::vector<f32> frameCoverage =
                SingleFrameCoverage(segments, width, height, mode, parameters, frame);
            for (sizet i = 0; i < pixelCount; ++i)
            {
                accumulated[i] += static_cast<f64>(frameCoverage[i]);
            }
        }

        const f64 invFrames = 1.0 / static_cast<f64>(parameters.TemporalFrames);
        std::vector<f32> coverage(pixelCount, 0.0f);
        for (sizet i = 0; i < pixelCount; ++i)
        {
            coverage[i] = static_cast<f32>(accumulated[i] * invFrames);
        }
        return coverage;
    }

    // -------------------------------------------------------------------------

    CoverageError CompareCoverage(const std::vector<f32>& measured, const std::vector<f32>& reference)
    {
        CoverageError error;
        if (measured.size() != reference.size() || measured.empty())
        {
            return error;
        }

        f64 sumAbsolute = 0.0;
        f64 sumSquared = 0.0;
        f64 sumSigned = 0.0;
        for (sizet i = 0; i < measured.size(); ++i)
        {
            const f64 m = static_cast<f64>(measured[i]);
            const f64 r = static_cast<f64>(reference[i]);
            if (r > 0.0)
            {
                ++error.ReferenceCoveredPixels;
            }
            if (m <= 0.0 && r <= 0.0)
            {
                continue;
            }
            const f64 difference = m - r;
            const f64 absolute = std::abs(difference);
            sumAbsolute += absolute;
            sumSquared += difference * difference;
            sumSigned += difference;
            error.MaxAbsolute = std::max(error.MaxAbsolute, absolute);
            ++error.ComparedPixels;
        }

        if (error.ComparedPixels > 0u)
        {
            const f64 inv = 1.0 / static_cast<f64>(error.ComparedPixels);
            error.MeanAbsolute = sumAbsolute * inv;
            error.Rmse = std::sqrt(sumSquared * inv);
            error.MeanSignedBias = sumSigned * inv;
        }
        return error;
    }

    f64 TotalCoverage(const std::vector<f32>& coverage)
    {
        f64 total = 0.0;
        for (const f32 value : coverage)
        {
            total += static_cast<f64>(value);
        }
        return total;
    }

    // -------------------------------------------------------------------------

    TemporalStability MeasureTemporalStability(const std::vector<ScreenSegment>& segments, u32 width, u32 height,
                                               GroomCompositionMode mode, const ModeParameters& parameters,
                                               u32 frameCount, const std::vector<f32>& reference)
    {
        TemporalStability stability;
        if (width == 0u || height == 0u || frameCount == 0u || !ParametersAreValid(mode, parameters))
        {
            return stability;
        }

        const sizet pixelCount = static_cast<sizet>(width) * static_cast<sizet>(height);
        std::vector<f64> accumulated(pixelCount, 0.0);
        std::vector<f32> previous;
        f64 sumOfMeanDeltas = 0.0;
        u32 deltaSamples = 0;

        for (u32 frame = 0; frame < frameCount; ++frame)
        {
            std::vector<f32> current = SingleFrameCoverage(segments, width, height, mode, parameters, frame);
            if (current.size() != pixelCount)
            {
                return stability;
            }

            for (sizet i = 0; i < pixelCount; ++i)
            {
                accumulated[i] += static_cast<f64>(current[i]);
            }

            if (!previous.empty())
            {
                f64 sumAbsolute = 0.0;
                u32 compared = 0;
                for (sizet i = 0; i < pixelCount; ++i)
                {
                    if (current[i] <= 0.0f && previous[i] <= 0.0f)
                    {
                        continue;
                    }
                    sumAbsolute += std::abs(static_cast<f64>(current[i]) - static_cast<f64>(previous[i]));
                    ++compared;
                }
                const f64 mean = compared > 0u ? sumAbsolute / static_cast<f64>(compared) : 0.0;
                sumOfMeanDeltas += mean;
                stability.MaxFrameToFrameDelta = std::max(stability.MaxFrameToFrameDelta, mean);
                ++deltaSamples;
            }

            previous = std::move(current);
            ++stability.FramesEvaluated;
        }

        if (deltaSamples > 0u)
        {
            stability.MeanFrameToFrameDelta = sumOfMeanDeltas / static_cast<f64>(deltaSamples);
        }

        if (reference.size() == pixelCount)
        {
            const f64 invFrames = 1.0 / static_cast<f64>(frameCount);
            std::vector<f32> converged(pixelCount, 0.0f);
            for (sizet i = 0; i < pixelCount; ++i)
            {
                converged[i] = static_cast<f32>(accumulated[i] * invFrames);
            }
            stability.ConvergedMeanAbsolute = CompareCoverage(converged, reference).MeanAbsolute;
        }
        return stability;
    }

    // -------------------------------------------------------------------------

    u64 ModeExtraBytes(GroomCompositionMode mode, u32 width, u32 height, u32 sampleCount,
                       u32 gbufferBytesPerSamplePerPixel)
    {
        const u64 pixels = static_cast<u64>(width) * static_cast<u64>(height);
        switch (mode)
        {
            case GroomCompositionMode::OpaqueRibbon:
            case GroomCompositionMode::StochasticAlpha:
                // Neither needs a target the frame does not already have: both
                // write colour and depth into whatever the scene pass bound.
                return 0ull;

            case GroomCompositionMode::AlphaToCoverage:
            {
                if (sampleCount <= 1u)
                {
                    return 0ull;
                }
                // The extra samples over the single-sample frame the other
                // modes render into. The resolve target is the single-sample
                // one that already existed, so it is not counted twice.
                return pixels * static_cast<u64>(sampleCount - 1u) * static_cast<u64>(gbufferBytesPerSamplePerPixel);
            }

            case GroomCompositionMode::WeightedBlendedOIT:
                // RGBA16F accumulation (8 B) + RG16F revealage (4 B). The
                // depth attachment of the OIT framebuffer is shared with the
                // scene's, so it adds nothing.
                return pixels * 12ull;

            case GroomCompositionMode::Count:
                break;
        }
        return 0ull;
    }
} // namespace OloEngine::GroomCoverage
