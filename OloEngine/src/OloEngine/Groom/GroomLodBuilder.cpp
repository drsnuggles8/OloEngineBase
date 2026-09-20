#include "OloEnginePCH.h"
#include "OloEngine/Groom/GroomLodBuilder.h"

#include "OloEngine/Groom/GroomCoat.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

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

        // ── Build ───────────────────────────────────────────────────────
        const u32 pointsPerCard = settings.PointsPerCard;
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
            for (sizet m = runStart; m < runEnd; ++m)
            {
                const u32 curve = members[m].Curve;
                for (u32 j = 0; j < pointsPerCard; ++j)
                {
                    const f32 t = pointsPerCard > 1u ? static_cast<f32>(j) / static_cast<f32>(pointsPerCard - 1u) : 0.0f;
                    const Sampled sample = SampleCurve(base, curve, t);
                    positionSum[j] += glm::dvec3(sample.Position);
                    widthSum[j] += static_cast<f64>(sample.Width);
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
                for (u32 p = 0; p < count; ++p)
                {
                    const f32 t = span > 0.0f ? static_cast<f32>(p) / span : 0.0f;
                    // The cluster's total width at this parameter, summed in the
                    // same fixed order as everything else here.
                    f64 clusterWidth = 0.0;
                    for (sizet m = runStart; m < runEnd; ++m)
                    {
                        clusterWidth += static_cast<f64>(SampleCurve(base, members[m].Curve, t).Width);
                    }
                    out.Points.push_back(base.GetPoints()[first + p]);
                    out.PointWidths.push_back(
                        static_cast<f32>(std::min(clusterWidth, static_cast<f64>(GroomLimits::MaxWidth))));
                    stats.MemberWidthSum += clusterWidth;
                    stats.CardWidthSum += static_cast<f64>(out.PointWidths.back());
                }
            }
            else
            {
                for (u32 j = 0; j < pointsPerCard; ++j)
                {
                    const glm::dvec3 mean = positionSum[j] * inverseCount;
                    out.Points.emplace_back(static_cast<f32>(mean.x), static_cast<f32>(mean.y),
                                            static_cast<f32>(mean.z));
                    // SUMMED, not averaged. That is the apparent-density
                    // contract: one band of the cluster's total width covers the
                    // area its members covered. Clamped to the format bound so a
                    // pathological cell cannot cook a width the reader would
                    // reject.
                    out.PointWidths.push_back(
                        static_cast<f32>(std::min(widthSum[j], static_cast<f64>(GroomLimits::MaxWidth))));
                    stats.MemberWidthSum += widthSum[j];
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

        if (!out.Validate(baseCurveCount, outReason))
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
