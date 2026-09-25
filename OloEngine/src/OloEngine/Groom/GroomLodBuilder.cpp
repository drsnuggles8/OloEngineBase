#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomLodBuilder.h"

#include "OloEngine/Groom/GroomCoat.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <span>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Authoring bounds, restated here rather than reached for through
        // GroomCoatLimits alone so the reason a card cell is bounded is written
        // where the card is built: below a texel of a 4096 map every strand is
        // its own card (no reduction at all), and above a quarter of the pelt
        // the whole animal is one card.
        constexpr u32 kMinPointsPerCard = 2;
        constexpr u32 kMaxPointsPerCard = 64;

        // One strand's membership of one card. The sort key is (group, cell,
        // curve) — a TOTAL order ending in the curve index, so two strands can
        // never compare equal and the sort's result cannot depend on the
        // algorithm's stability. GroomCooker.h's determinism contract is what
        // makes that worth spelling out.
        struct Member
        {
            u16 Group = 0;
            u64 Cell = 0;
            u32 Curve = 0;

            [[nodiscard]] bool operator<(const Member& other) const noexcept
            {
                if (Group != other.Group)
                {
                    return Group < other.Group;
                }
                if (Cell != other.Cell)
                {
                    return Cell < other.Cell;
                }
                return Curve < other.Curve;
            }

            [[nodiscard]] bool SameCard(const Member& other) const noexcept
            {
                return Group == other.Group && Cell == other.Cell;
            }
        };

        // The point on `curve`'s polyline at parameter `t` in [0,1], where the
        // parameter runs over CONTROL POINT INDEX rather than arc length.
        //
        // Index parameter, deliberately: it is the same `t` the coat's shape
        // term, the guide displacement sample and the strand vertex's Coords.x
        // all use (see GroomStrandMesh.cpp's `invSpan`). Arc length would be a
        // better resampling in isolation and a WORSE one here — a card built on
        // one parameterisation and shaded, clumped and simulated on another
        // would drift against its own members wherever a strand's spacing is
        // uneven, which is every strand that droops.
        struct Sampled
        {
            glm::vec3 Position{ 0.0f };
            f32 Width = 0.0f;
        };

        [[nodiscard]] Sampled SampleCurve(const GroomAsset& groom, u32 curve, f32 t) noexcept
        {
            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 count = groom.GetCurvePointCount(curve);
            const auto& points = groom.GetPoints();
            const auto& widths = groom.GetPointWidths();

            const f32 span = static_cast<f32>(count - 1u);
            const f32 position = std::clamp(t, 0.0f, 1.0f) * span;
            // floor() then a clamp to the last SEGMENT, so t == 1 lands on the
            // final segment at fraction 1 rather than indexing one past the end.
            const auto lower = static_cast<u32>(std::min(std::floor(position), span - 1.0f > 0.0f ? span - 1.0f : 0.0f));
            const u32 upper = std::min(lower + 1u, count - 1u);
            const f32 fraction = position - static_cast<f32>(lower);

            Sampled out;
            out.Position = glm::mix(points[first + lower], points[first + upper], fraction);
            out.Width = glm::mix(widths[first + lower], widths[first + upper], fraction);
            return out;
        }

        // ── The covered width (#1428) ───────────────────────────────────
        //
        // A card is seen from every side, and the members it replaces cover a
        // different width from each: a flat fan of strands is wide face-on and
        // one strand wide edge-on. The card gets the AVERAGE over directions
        // around it -- eight across a half turn, because a band seen from the
        // opposite side covers the same interval. A ribbon faces the camera,
        // so a member's band is its own width whichever way it is seen; only
        // where its centre lands across the card changes.
        constexpr u32 kCoveredWidthDirections = 8;

        struct CoveredWidthScratch
        {
            struct Edge
            {
                f32 At = 0.0f;
                // +1 opens a band, -1 closes it.
                i32 Sign = 0;
                // log(1 - alpha) of a see-through band; 0 for an opaque one,
                // which Opaque counts instead (the log would be -infinity).
                f64 LogClear = 0.0;
                bool Opaque = false;

                [[nodiscard]] bool operator<(const Edge& other) const noexcept
                {
                    return At < other.At;
                }
            };

            std::vector<glm::vec3> Positions;
            std::vector<f32> Widths;
            std::vector<glm::vec2> Offsets;
            std::vector<Edge> Edges;

            void Clear() noexcept
            {
                Positions.clear();
                Widths.clear();
            }
        };

        [[nodiscard]] f64 SummedWidth(const CoveredWidthScratch& scratch) noexcept
        {
            f64 summed = 0.0;
            for (const f32 width : scratch.Widths)
            {
                summed += static_cast<f64>(width);
            }
            return summed;
        }

        // The width the members in `scratch` cover across a card through
        // `centre` running along `tangent`, AS THE STRAND SHADER DRAWS THEM
        // when one pixel spans `footprint` of groom space, averaged over
        // kCoveredWidthDirections.
        //
        // The shader widens a strand thinner than a pixel to one pixel and
        // scales its alpha by how much it widened it, and the stochastic mode
        // drops each strand's fragments independently, so where bands overlap
        // they cover 1 - prod(1 - alpha). That is the covered width here: the
        // integral of that coverage across the card. At a zero footprint it is
        // the exact union of the bands; as the footprint grows past the whole
        // cluster it tends to their sum. A card is one band, so the width it
        // needs is exactly this number, at any footprint.
        //
        // Returns the SUM when the card has no direction (a zero-length
        // tangent), which is the old answer rather than a guess at a new one.
        [[nodiscard]] f64 CoveredWidth(CoveredWidthScratch& scratch, const glm::vec3& centre, const glm::vec3& tangent,
                                       f32 footprint)
        {
            const f64 summed = SummedWidth(scratch);
            const f32 tangentLength = glm::length(tangent);
            if (scratch.Positions.size() < 2u || !std::isfinite(tangentLength) || !(tangentLength > 1.0e-12f))
            {
                return summed;
            }
            const glm::vec3 along = tangent / tangentLength;
            // Any two axes perpendicular to the card. The choice only rotates
            // which direction is sampled first, and the eight are evenly spread.
            const glm::vec3 helper =
                std::abs(along.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 u = glm::normalize(glm::cross(along, helper));
            const glm::vec3 v = glm::cross(along, u);

            scratch.Offsets.clear();
            for (const glm::vec3& position : scratch.Positions)
            {
                const glm::vec3 offset = position - centre;
                scratch.Offsets.emplace_back(glm::dot(offset, u), glm::dot(offset, v));
            }

            f64 covered = 0.0;
            for (u32 d = 0; d < kCoveredWidthDirections; ++d)
            {
                const f32 angle = glm::pi<f32>() * static_cast<f32>(d) / static_cast<f32>(kCoveredWidthDirections);
                const glm::vec2 across{ std::cos(angle), std::sin(angle) };
                scratch.Edges.clear();
                for (sizet m = 0; m < scratch.Offsets.size(); ++m)
                {
                    const f32 width = scratch.Widths[m];
                    if (!(width > 0.0f))
                    {
                        continue;
                    }
                    const f32 x = glm::dot(scratch.Offsets[m], across);
                    const bool opaque = !(width < footprint);
                    const f32 half = (opaque ? width : footprint) * 0.5f;
                    const f64 logClear = opaque ? 0.0 : std::log1p(-static_cast<f64>(width) / footprint);
                    scratch.Edges.push_back({ x - half, +1, logClear, opaque });
                    scratch.Edges.push_back({ x + half, -1, logClear, opaque });
                }
                if (scratch.Edges.empty())
                {
                    continue;
                }
                // Stable, so two edges at one position keep the order they
                // were pushed in and the sum below is a function of the groom.
                // The width between them is zero either way.
                std::stable_sort(scratch.Edges.begin(), scratch.Edges.end());

                f64 length = 0.0;
                i32 opaqueOpen = 0;
                f64 logClear = 0.0;
                f32 previous = scratch.Edges.front().At;
                for (const CoveredWidthScratch::Edge& edge : scratch.Edges)
                {
                    const f64 span = static_cast<f64>(edge.At - previous);
                    if (span > 0.0)
                    {
                        const f64 coverage = opaqueOpen > 0 ? 1.0 : -std::expm1(logClear);
                        length += span * coverage;
                    }
                    previous = edge.At;
                    if (edge.Opaque)
                    {
                        opaqueOpen += edge.Sign;
                    }
                    else
                    {
                        logClear += static_cast<f64>(edge.Sign) * edge.LogClear;
                    }
                }
                covered += length;
            }
            covered /= static_cast<f64>(kCoveredWidthDirections);
            // Never more than the sum: a card wider than its members is the
            // exact defect this exists to remove, and f32 offsets can put the
            // integral a rounding error over.
            return std::min(covered, summed);
        }

        // Central difference along a polyline's points, one-sided at the ends.
        [[nodiscard]] glm::vec3 PolylineTangent(std::span<const glm::vec3> points, u32 p) noexcept
        {
            const auto last = static_cast<u32>(points.size() - 1u);
            const u32 before = p > 0u ? p - 1u : 0u;
            const u32 after = p < last ? p + 1u : last;
            return points[after] - points[before];
        }
    } // namespace

    bool GroomLodBuilder::BuildCardLevel(const GroomAsset& base, const GroomCardSettings& settings, GroomLodLevel& out,
                                         std::string& outReason, GroomCardBuildStats* outStats)
    {
        out = GroomLodLevel{};
        GroomCardBuildStats stats;

        // ── The settings, validated rather than clamped ─────────────────
        //
        // A card level is a cooked artifact: silently repairing the settings
        // would put a level on disk that nobody asked for and that nothing
        // records the parameters of. The cook is a place where a named refusal
        // is cheap and a wrong answer is permanent.
        if (!std::isfinite(settings.CellSize) || settings.CellSize < GroomCoatLimits::MinClumpCellSize ||
            settings.CellSize > GroomCoatLimits::MaxClumpCellSize)
        {
            outReason = std::format("card cell size {} is outside [{}, {}]", settings.CellSize,
                                    GroomCoatLimits::MinClumpCellSize, GroomCoatLimits::MaxClumpCellSize);
            return false;
        }
        if (settings.Width >= GroomCardWidth::Count)
        {
            outReason = std::format("card width model {} is not one of the {} known", static_cast<u32>(settings.Width),
                                    static_cast<u32>(GroomCardWidth::Count));
            return false;
        }
        if (settings.PointsPerCard < kMinPointsPerCard || settings.PointsPerCard > kMaxPointsPerCard)
        {
            outReason = std::format("points per card {} is outside [{}, {}]", settings.PointsPerCard,
                                    kMinPointsPerCard, kMaxPointsPerCard);
            return false;
        }
        if (!std::isfinite(settings.SourcePixelSize) || settings.SourcePixelSize < 0.0f)
        {
            outReason = "card source pixel size is not a finite, non-negative number";
            return false;
        }
        // MaxCards is validated like the other three, and for the same reason:
        // it is the one setting that SIZES AN ALLOCATION. The reserve below is
        // `cardCount * PointsPerCard` points, so an unbounded cap admits a
        // reserve of MaxCurveCount x 64 points -- about half a billion -- long
        // before Validate ever sees the level. A cook that refuses by name costs
        // nothing; a std::bad_alloc from a tool's typo is not something any
        // caller of this function catches.
        if (settings.MaxCards == 0u || settings.MaxCards > GroomLimits::MaxCurveCount)
        {
            outReason = std::format("card cap {} is outside [1, {}]", settings.MaxCards,
                                    GroomLimits::MaxCurveCount);
            return false;
        }

        const u32 baseCurveCount = base.GetCurveCount();
        if (baseCurveCount == 0)
        {
            outReason = "base groom has no curves";
            return false;
        }

        // ── The root parameterisation has to be addressable ─────────────
        //
        // GroomCoatClumpCell CLAMPS a root UV to +/-16 before quantising it,
        // so a groom whose chart runs past that has every strand beyond the
        // clamp land in ONE cell. The cook would then "succeed", produce a
        // handful of cards each standing in for thousands of strands, and the
        // coat would lose four fifths of its covered area the instant it handed
        // over — a plausible-looking level that is quietly wrong, which is
        // exactly the class of failure this issue's confidence-0.5 rating is
        // about. It was found by measurement, not by inspection: a test fixture
        // whose root U ran to 11459 cooked 64 cards for a 20 000-strand scalp
        // and carried 16% of its coverage.
        //
        // NAMED AND REFUSED rather than clamped. A groom with a UDIM chart is a
        // real thing and this is a real limitation of the clump-cell addressing
        // (which #1251 already has, silently); saying so is what lets someone
        // fix the export or the addressing, and a silently useless level is
        // what stops them ever finding out.
        constexpr f32 kMaxAddressableUV = 16.0f;
        for (u32 curve = 0; curve < baseCurveCount; ++curve)
        {
            const glm::vec2& uv = base.GetRootUVs()[curve];
            if (!std::isfinite(uv.x) || !std::isfinite(uv.y) || std::abs(uv.x) > kMaxAddressableUV ||
                std::abs(uv.y) > kMaxAddressableUV)
            {
                outReason =
                    std::format("curve {} has root UV ({}, {}), outside the +/-{} the clump-cell addressing can "
                                "separate; every strand past that bound falls in one cell, so the cards would "
                                "carry a fraction of the coat's density. Re-export the groom with its root UVs "
                                "inside the unit chart.",
                                curve, uv.x, uv.y, kMaxAddressableUV);
                return false;
            }
        }

        // ── Cluster ─────────────────────────────────────────────────────
        std::vector<Member> members;
        members.reserve(baseCurveCount);
        const auto& rootUVs = base.GetRootUVs();
        const auto& groupIds = base.GetCurveGroupIds();
        for (u32 curve = 0; curve < baseCurveCount; ++curve)
        {
            if (base.GetCurvePointCount(curve) < GroomLimits::MinPointsPerCurve)
            {
                ++stats.CurvesSkippedTooShort;
                continue;
            }
            ++stats.CurvesConsidered;
            members.push_back(Member{ groupIds[curve], GroomCoatClumpCell(rootUVs[curve], settings.CellSize), curve });
        }
        if (members.empty())
        {
            outReason = "no curve in the base groom has at least two control points";
            return false;
        }
        std::sort(members.begin(), members.end());

        // Count the cards before building any, so MaxCards is a refusal rather
        // than a truncation partway through the pelt.
        u32 cardCount = 0;
        for (sizet i = 0; i < members.size(); ++i)
        {
            if (i == 0 || !members[i].SameCard(members[i - 1]))
            {
                ++cardCount;
            }
        }
        if (cardCount > settings.MaxCards)
        {
            outReason = std::format("card cell {} produces {} cards, above the cap {}; use a larger cell",
                                    settings.CellSize, cardCount, settings.MaxCards);
            return false;
        }
        if (cardCount >= stats.CurvesConsidered)
        {
            // A level that reduces nothing costs disk, load time and resident
            // memory to draw exactly what the strand tier draws. Refused by
            // name rather than cooked: a groom whose coarse tier is the same
            // size as its fine one is an authoring mistake that would otherwise
            // only show up as a memory figure nobody reads.
            outReason = std::format("card cell {} produces {} cards from {} strands, which is no reduction; "
                                    "use a larger cell",
                                    settings.CellSize, cardCount, stats.CurvesConsidered);
            return false;
        }

        // The same bound on the POINTS the build is about to reserve. Checked
        // against the cards actually counted rather than against MaxCards, so a
        // generous cap costs nothing on a groom that does not reach it.
        if (static_cast<u64>(cardCount) * settings.PointsPerCard > GroomLimits::MaxPointCount)
        {
            outReason = std::format("card cell {} produces {} cards of {} points, above the format's {}-point cap",
                                    settings.CellSize, cardCount, settings.PointsPerCard,
                                    GroomLimits::MaxPointCount);
            return false;
        }

        // ── Build ───────────────────────────────────────────────────────
        const u32 pointsPerCard = settings.PointsPerCard;

        // One pixel at the hand-over, in groom space. The LOD's apparent size is
        // the groom's largest bounding extent in pixels (EstimateProjectedPixelSize),
        // so when the coat is SourcePixelSize pixels across a pixel spans this
        // much of it. Zero when the cook was told no size, and then the covered
        // width is the exact union of the members' bands.
        const glm::vec3 extent = base.GetBoundsMax() - base.GetBoundsMin();
        const f32 largestExtent = std::max({ extent.x, extent.y, extent.z });
        const f32 footprint = settings.SourcePixelSize > 0.0f && std::isfinite(largestExtent) && largestExtent > 0.0f
                                  ? largestExtent / settings.SourcePixelSize
                                  : 0.0f;
        stats.PixelFootprint = footprint;
        out.Representation = GroomRepresentation::Card;
        out.SourcePixelSize = settings.SourcePixelSize;
        out.CurveOffsets.reserve(static_cast<sizet>(cardCount) + 1u);
        out.Points.reserve(static_cast<sizet>(cardCount) * pointsPerCard);
        out.PointWidths.reserve(static_cast<sizet>(cardCount) * pointsPerCard);
        out.RootUVs.reserve(cardCount);
        out.CurveGroupIds.reserve(cardCount);
        out.CurveFlags.reserve(cardCount);
        out.SourceCurves.reserve(cardCount);
        out.CurveOffsets.push_back(0u);

        std::vector<glm::dvec3> positionSum(pointsPerCard);
        std::vector<f64> widthSum(pointsPerCard);
        std::vector<glm::vec3> meanPoints(pointsPerCard);
        CoveredWidthScratch covered;
        const bool coveredWidth = settings.Width == GroomCardWidth::Covered;

        stats.SmallestCluster = std::numeric_limits<u32>::max();

        sizet runStart = 0;
        while (runStart < members.size())
        {
            sizet runEnd = runStart + 1;
            while (runEnd < members.size() && members[runEnd].SameCard(members[runStart]))
            {
                ++runEnd;
            }
            const auto memberCount = static_cast<u32>(runEnd - runStart);

            std::fill(positionSum.begin(), positionSum.end(), glm::dvec3(0.0));
            std::fill(widthSum.begin(), widthSum.end(), 0.0);
            glm::dvec2 rootUVSum{ 0.0 };
            glm::dvec3 rootSum{ 0.0 };

            // f64 accumulation in the sorted order. The order is what makes the
            // sum reproducible; the width is what makes it MEANINGFUL, because
            // f32 accumulation over a thousand 7e-5 diameters loses the tail of
            // the cluster and the card comes out systematically thin.
            //
            // The per-point sums are MeanCentreline's alone; RepresentativeStrand
            // resamples the cluster at its member's own parameters below and
            // would only be paying for a walk it discards. The root sums are
            // both modes' — the representative is chosen from them.
            const bool needsMeanSums = settings.Aggregation == GroomCardAggregation::MeanCentreline;
            for (sizet m = runStart; m < runEnd; ++m)
            {
                const u32 curve = members[m].Curve;
                if (needsMeanSums)
                {
                    for (u32 j = 0; j < pointsPerCard; ++j)
                    {
                        const f32 t =
                            pointsPerCard > 1u ? static_cast<f32>(j) / static_cast<f32>(pointsPerCard - 1u) : 0.0f;
                        const Sampled sample = SampleCurve(base, curve, t);
                        positionSum[j] += glm::dvec3(sample.Position);
                        widthSum[j] += static_cast<f64>(sample.Width);
                    }
                }
                rootUVSum += glm::dvec2(rootUVs[curve]);
                rootSum += glm::dvec3(base.GetPoints()[base.GetCurveFirstPoint(curve)]);
            }

            const f64 inverseCount = 1.0 / static_cast<f64>(memberCount);
            const glm::dvec3 meanRoot = rootSum * inverseCount;

            // The REPRESENTATIVE: the member whose root is nearest the
            // cluster's mean root, ties broken by the lower curve index (the
            // members are already sorted by it, and the comparison is strict).
            // Its rest frame and its guides are what the card borrows, so
            // picking the geometrically central member is what keeps a card
            // attached where its tuft actually grows rather than at whichever
            // edge strand happened to sort first.
            u32 representative = members[runStart].Curve;
            f64 bestDistanceSq = std::numeric_limits<f64>::max();
            for (sizet m = runStart; m < runEnd; ++m)
            {
                const u32 curve = members[m].Curve;
                const glm::dvec3 root{ base.GetPoints()[base.GetCurveFirstPoint(curve)] };
                const glm::dvec3 delta = root - meanRoot;
                const f64 distanceSq = glm::dot(delta, delta);
                if (distanceSq < bestDistanceSq)
                {
                    bestDistanceSq = distanceSq;
                    representative = curve;
                }
            }

            if (settings.Aggregation == GroomCardAggregation::RepresentativeStrand)
            {
                // THE MEMBER ITSELF, kept verbatim, with its widths scaled so
                // it carries the cluster's total width.
                //
                // The scale is (cluster width at this parameter) / (the
                // representative's width at the same parameter), evaluated per
                // POINT rather than once per card: a cluster of strands that
                // taper at different rates has a ratio that varies along the
                // strand, and one scalar would make the card too fat at the
                // root or too thin at the tip.
                const u32 first = base.GetCurveFirstPoint(representative);
                const u32 count = base.GetCurvePointCount(representative);
                const f32 span = static_cast<f32>(count - 1u);
                const std::span<const glm::vec3> ownPoints{ base.GetPoints().data() + first, count };
                for (u32 p = 0; p < count; ++p)
                {
                    const f32 t = span > 0.0f ? static_cast<f32>(p) / span : 0.0f;
                    // Every member at this parameter, in the same fixed order
                    // as everything else here.
                    covered.Clear();
                    for (sizet m = runStart; m < runEnd; ++m)
                    {
                        const Sampled sample = SampleCurve(base, members[m].Curve, t);
                        covered.Positions.push_back(sample.Position);
                        covered.Widths.push_back(sample.Width);
                    }
                    const f64 summedWidth = SummedWidth(covered);
                    const f64 coveredClusterWidth =
                        CoveredWidth(covered, ownPoints[p], PolylineTangent(ownPoints, p), footprint);
                    const f64 clusterWidth = coveredWidth ? coveredClusterWidth : summedWidth;
                    out.Points.push_back(ownPoints[p]);
                    out.PointWidths.push_back(
                        static_cast<f32>(std::min(clusterWidth, static_cast<f64>(GroomLimits::MaxWidth))));
                    stats.MemberWidthSum += summedWidth;
                    stats.MemberCoveredWidthSum += coveredClusterWidth;
                    stats.CardWidthSum += static_cast<f64>(out.PointWidths.back());
                }
            }
            else
            {
                for (u32 j = 0; j < pointsPerCard; ++j)
                {
                    const glm::dvec3 mean = positionSum[j] * inverseCount;
                    meanPoints[j] =
                        glm::vec3(static_cast<f32>(mean.x), static_cast<f32>(mean.y), static_cast<f32>(mean.z));
                }
                for (u32 j = 0; j < pointsPerCard; ++j)
                {
                    const f32 t =
                        pointsPerCard > 1u ? static_cast<f32>(j) / static_cast<f32>(pointsPerCard - 1u) : 0.0f;
                    covered.Clear();
                    for (sizet m = runStart; m < runEnd; ++m)
                    {
                        const Sampled sample = SampleCurve(base, members[m].Curve, t);
                        covered.Positions.push_back(sample.Position);
                        covered.Widths.push_back(sample.Width);
                    }
                    const f64 coveredClusterWidth =
                        CoveredWidth(covered, meanPoints[j], PolylineTangent(meanPoints, j), footprint);
                    // SUMMED or COVERED, never averaged: that is the
                    // apparent-density contract, one band carrying the width
                    // its members carried. Clamped to the format bound so a
                    // pathological cell cannot cook a width the reader would
                    // reject.
                    const f64 clusterWidth = coveredWidth ? coveredClusterWidth : widthSum[j];
                    out.Points.push_back(meanPoints[j]);
                    out.PointWidths.push_back(
                        static_cast<f32>(std::min(clusterWidth, static_cast<f64>(GroomLimits::MaxWidth))));
                    stats.MemberWidthSum += widthSum[j];
                    stats.MemberCoveredWidthSum += coveredClusterWidth;
                    stats.CardWidthSum += static_cast<f64>(out.PointWidths.back());
                }
            }

            const glm::dvec2 meanUV = rootUVSum * inverseCount;
            out.RootUVs.emplace_back(static_cast<f32>(meanUV.x), static_cast<f32>(meanUV.y));
            out.CurveGroupIds.push_back(members[runStart].Group);
            // The guide flag follows the representative, so the editor's
            // guides-only view still shows something at card range. A card is
            // not itself an authored guide and nothing simulates it as one —
            // the guide it FOLLOWS is its representative's, through SourceCurves.
            out.CurveFlags.push_back(base.GetCurveFlags()[representative]);
            out.SourceCurves.push_back(representative);
            out.CurveOffsets.push_back(static_cast<u32>(out.Points.size()));

            stats.SmallestCluster = std::min(stats.SmallestCluster, memberCount);
            stats.LargestCluster = std::max(stats.LargestCluster, memberCount);
            ++stats.CardsBuilt;
            runStart = runEnd;
        }

        stats.MeanCluster =
            stats.CardsBuilt > 0u ? static_cast<f32>(stats.CurvesConsidered) / static_cast<f32>(stats.CardsBuilt) : 0.0f;
        if (stats.CardsBuilt == 0u)
        {
            stats.SmallestCluster = 0;
        }

        if (!out.Validate(baseCurveCount, base.GetGroupCount(), outReason))
        {
            out = GroomLodLevel{};
            return false;
        }

        if (outStats != nullptr)
        {
            *outStats = stats;
        }
        return true;
    }

    bool GroomLodBuilder::AttachLodLevels(GroomAsset& groom, std::vector<GroomLodLevel> levels, std::string& outReason)
    {
        // Swapped in, validated, and swapped BACK OUT on failure. A groom left
        // holding a level that does not validate is a groom that indexes the
        // binding's root-transform array out of bounds in the renderer's
        // innermost loop — so the failure path has to restore, not merely
        // report.
        std::vector<GroomLodLevel> previous = std::move(groom.m_LodLevels);
        groom.m_LodLevels = std::move(levels);
        if (!groom.Validate(outReason))
        {
            groom.m_LodLevels = std::move(previous);
            return false;
        }
        return true;
    }
} // namespace OloEngine
