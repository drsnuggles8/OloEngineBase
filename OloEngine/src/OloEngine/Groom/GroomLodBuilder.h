#pragma once

// =============================================================================
// GroomLodBuilder.h — cooking a groom's coarser representations. Issue #1252.
//
// WHAT A CARD LEVEL IS, AS A CONSTRUCTION. Every strand is assigned to the
// CLUMP CELL its root UV falls in -- the same quantisation #1251 already forms
// tufts over, so a card and a clump are the same patch of pelt rather than two
// unrelated partitions of it -- and each (group, cell) becomes ONE CURVE: the
// cluster's most central member, kept verbatim, with its widths scaled so it
// carries the cluster's TOTAL width.
//
// WHY A KEPT MEMBER AND NOT AN AVERAGE, which is what the first version built.
// A mean centreline is the textbook hair card and it MEASURED WORSE on every
// coat at every distance: averaging curves that diverge produces a shorter,
// straighter curve, so it loses exactly the spread that gives a tuft its
// silhouette, and on the short coat it lost 45% of the covered area outright
// (0.548 against the kept member's 0.963). A kept strand is a SAMPLE of the
// coat and lies on its manifold by construction. GroomCardAggregation keeps
// both, and MeanCentreline survives as the measured-and-rejected alternative
// the way GroomCompositionMode::AlphaToCoverage does.
//
// WHY THE SUMMED WIDTH IS THE WHOLE POINT, and why it is what makes this tier
// worth cooking at all. A strand covers (projected length x projected width),
// so a cluster's covered area is the sum of its members' widths; one band of
// that summed width covers the same area, at every distance, including below
// one pixel where the widened-alpha lane carries it (groom-strand-visibility.md
// rule 2).
//
// The runtime strand budget can do the same arithmetic for free -- thin by k,
// widen by 1/k -- and at modest reductions it does, within noise. But the
// runtime widening is CAPPED (GroomLodPolicy::MaxWidthCompensation, 8x by
// default) because a strand widened sixty times is not a strand, it is a flat
// band that reads as a different material. Past that cap a stride CANNOT
// restore the density: at a 59x reduction the matched strand arm carries 0.23
// of the coat's area while the card carries 1.01. That is the gap this tier
// exists in, it is the reason a level is cooked rather than derived, and it is
// measured rather than argued -- docs/analysis/groom-representation-lod-1252.md.
//
//   * The GROUP IS NEVER CROSSED. A cell holding undercoat and guard hair
//     produces two cards, not one, so a card keeps its group's role, its coat
//     description and its budget weight. Merging them would average a dense
//     short fibre with a sparse long one and produce a tier whose silhouette is
//     neither -- which is #1251's "uniformly fuzzy coat" reintroduced at range.
//
// WHY THE LEVEL IS COOKED AND NOT BUILT AT LOAD. Clustering a 200k-strand groom
// is a sort plus two walks; doing it per load, per process, on the main thread
// is a stall an artist would feel on every scene open. It is also a DERIVED
// artifact of exactly the kind .ologroom already exists to hold, and cooking it
// is what makes "LOD assets are authored/cooked" true rather than a paraphrase
// of "the runtime decimates".
//
// DETERMINISM IS THE SAME CONTRACT THE COOK ALREADY HAS (GroomCooker.h). The
// cluster table is a SORTED VECTOR, never an iterated hash map; the sort key
// ends in the curve index so it is a total order; every sum is accumulated in
// that fixed order. Cooking a groom twice produces byte-identical levels, and
// GroomCookDeterminismTest covers them because it compares the whole file.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // How a cluster's members become one curve.
    //
    // TWO CANDIDATES, because "cards beat a budget stride" is not obvious and
    // had to be measured. The difference between them is the whole question:
    // one AVERAGES its members, the other KEEPS one of them.
    enum class GroomCardAggregation : u8
    {
        /// The card's centreline is the MEAN of its members', resampled to a
        /// fixed point count, and its width is their sum. The textbook hair
        /// card minus the texture.
        MeanCentreline = 0,

        /// The card IS its cluster's most central member, kept verbatim, with
        /// its widths scaled so it carries the cluster's total width.
        ///
        /// A REAL STRAND rather than an average, which is the property that
        /// turns out to matter: averaging curves that diverge produces a
        /// shorter, straighter curve, so a mean centreline loses exactly the
        /// spread that gives a tuft its silhouette. A kept strand is a SAMPLE
        /// of the coat and is on its manifold by construction.
        ///
        /// THE ONE THE MEASURED COMPARISON SELECTED, on every coat and at every
        /// distance: on the human scalp at a 59x reduction it scored a
        /// per-pixel silhouette error of 0.054-0.098 against the mean
        /// centreline's 0.072-0.115, and it costs FEWER segments on a short
        /// coat because it keeps its member's own point count instead of
        /// resampling to a fixed one.
        RepresentativeStrand = 1,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomCardAggregation aggregation) noexcept
    {
        switch (aggregation)
        {
            case GroomCardAggregation::MeanCentreline:
                return "MeanCentreline";
            case GroomCardAggregation::RepresentativeStrand:
                return "RepresentativeStrand";
            case GroomCardAggregation::Count:
                break;
        }
        return "MeanCentreline";
    }

    // How wide a card is: what its cluster's members COVER as the renderer draws
    // them at the hand-over, or what they would cover if none overlapped.
    //
    // TWO CANDIDATES, and the second one is the fix for #1428. The first
    // version summed the members' widths, which is exact only while no member
    // hides behind another. Strands that share a clump cell grow from roots a
    // few millimetres apart and are combed the same way, so on a dense coat
    // they overlap heavily: on the long-coated horse the long-hair role's
    // members cover 0.40 of their summed width, and the summed card tier drew
    // 1.35-1.9x the coat's share of the animal past the hand-over.
    //
    // THE PIXEL IS PART OF THE ANSWER, and the exact geometric union is the
    // wrong target without it. Past the hand-over a strand is thinner than a
    // pixel; the strand shader widens it to one pixel at an alpha of its true
    // width, and overlapping strands then cover 1 - prod(1 - alpha), as if
    // they were independent. So a lock of strands that geometrically covers one
    // strand's width is DRAWN wider than that, by an amount set by the pixel.
    // The exact union made the card tier keep 0.65 of the coat's share; the
    // sum made it keep 1.92. The card needs what the strands are drawn as.
    enum class GroomCardWidth : u8
    {
        /// The sum of the members' widths: the covered width of a cluster in
        /// which no member overlaps another, and what every cluster tends to
        /// as the pixel grows past it. The measured-and-rejected alternative.
        Summed = 0,

        /// The width the members cover AS THE STRAND SHADER DRAWS THEM when
        /// the coat is GroomCardSettings::SourcePixelSize pixels across: each
        /// member a band of its own width at its own offset from the card,
        /// widened to a pixel at a proportional alpha if it is thinner, the
        /// bands composited as independent layers, integrated across the card
        /// and averaged over the directions it can be seen from. The exact
        /// geometric union when SourcePixelSize is zero; never more than the
        /// sum; the sum itself when no two members overlap -- which is why the
        /// reference coats #1252 measured barely moved.
        Covered = 1,

        Count
    };

    [[nodiscard]] constexpr std::string_view ToString(GroomCardWidth width) noexcept
    {
        switch (width)
        {
            case GroomCardWidth::Summed:
                return "Summed";
            case GroomCardWidth::Covered:
                return "Covered";
            case GroomCardWidth::Count:
                break;
        }
        return "Summed";
    }

    // How a card level is formed. Every field is an AUTHORING choice with a
    // visible consequence, which is why none of them is a magic number inside
    // the builder.
    struct GroomCardSettings
    {
        /// Which of the two constructions above. The default is the one the
        /// measured comparison selected — see
        /// docs/analysis/groom-representation-lod-1252.md.
        GroomCardAggregation Aggregation = GroomCardAggregation::RepresentativeStrand;

        /// How wide each card is. The default is the one #1428 measured.
        GroomCardWidth Width = GroomCardWidth::Covered;

        /// Root-UV cell edge one card is formed over. This is the knob: a
        /// larger cell means fewer, fatter cards and a coarser silhouette.
        /// Shares GroomCoatLimits' clump-cell bounds deliberately — a card and
        /// a clump are the same patch of pelt.
        f32 CellSize = 0.05f;

        /// Control points on a card's centreline. Members are resampled onto
        /// this many uniform parameter values before they are averaged, so
        /// strands of different control-point counts still average
        /// root-to-root and tip-to-tip.
        ///
        /// IGNORED by RepresentativeStrand, which keeps its member's own
        /// points: resampling a strand it is not averaging would only add
        /// error, and it would make the card cost more segments than the
        /// strands it replaced — which is one of the two ways MeanCentreline
        /// lost the comparison.
        u32 PointsPerCard = 6;

        /// The coat's apparent size, in pixels across its largest extent, at
        /// which the cook expects the card tier to take over. Recorded on the
        /// level, and it sets the pixel GroomCardWidth::Covered measures at:
        /// one pixel spans (largest extent / SourcePixelSize) of the groom.
        /// GroomLodPolicy still decides where the hand-over happens, so keep
        /// this equal to the policy's CardPixelSize (both default to 256); a
        /// card is only width-matched to the strands at the size it was cooked
        /// for. Zero measures the exact geometric union instead.
        f32 SourcePixelSize = 256.0f;

        /// Upper bound on the cards produced. EXCEEDING IT IS AN ERROR, NOT A
        /// TRUNCATION: truncating would delete whichever region of the pelt
        /// sorted last, which is a bald flank — the exact failure
        /// groom-strand-visibility.md rule 7 gives the strand budget its stride
        /// to avoid. The fix for a groom that trips this is a larger cell.
        u32 MaxCards = 200000;
    };

    // What the cook produced, for the log line and the PR's evidence table.
    struct GroomCardBuildStats
    {
        u32 CurvesConsidered = 0;
        u32 CurvesSkippedTooShort = 0;
        u32 CardsBuilt = 0;
        /// Members per card: the reduction factor, as the three numbers that
        /// explain it rather than one mean that hides a bimodal coat.
        u32 SmallestCluster = 0;
        u32 LargestCluster = 0;
        f32 MeanCluster = 0.0f;
        /// Summed cooked width of every member, the width those members
        /// COVER as drawn at the hand-over (GroomCardWidth::Covered), and the
        /// width every card carries,
        /// all sampled at the parameters the emitted card uses. The card sum
        /// EQUALS the member sum under Summed and the covered sum under
        /// Covered, to within float accumulation and the format's width clamp,
        /// and it is asserted rather than assumed: this IS the apparent-density
        /// claim of criterion 1, and a clustering bug that dropped a member
        /// would show here first. Covered over Summed is how much the members
        /// overlap: 1 when none do.
        ///
        /// The two are NOT comparable between aggregations: MeanCentreline
        /// samples PointsPerCard uniform parameters and RepresentativeStrand
        /// samples its member's own points, so the same groom reports different
        /// totals under the two. Compare each against its own card sum, never
        /// one mode's against the other's.
        f64 MemberWidthSum = 0.0;
        f64 MemberCoveredWidthSum = 0.0;
        f64 CardWidthSum = 0.0;
        /// One pixel at the hand-over, in groom units: the footprint
        /// GroomCardWidth::Covered measured at. Zero means the exact union.
        f32 PixelFootprint = 0.0f;
    };

    class GroomLodBuilder
    {
      public:
        /// Builds the card level for `base` into `out`. Returns false with a
        /// named reason — never a partially-filled level.
        ///
        /// `base` is expected to be CANONICALISED (GroomCooker::Canonicalize),
        /// because the source map indexes its curves and a later reorder would
        /// silently repoint every card at a different strand. The cook calls
        /// this after canonicalisation for that reason; a caller that does not
        /// gets a level that is valid and wrong, which is why the cook is the
        /// only caller that matters.
        [[nodiscard]] static bool BuildCardLevel(const GroomAsset& base, const GroomCardSettings& settings,
                                                 GroomLodLevel& out, std::string& outReason,
                                                 GroomCardBuildStats* outStats = nullptr);

        /// Attaches `levels` to `groom`, replacing whatever it had, and
        /// re-validates. Returns false with a named reason and leaves the groom
        /// UNCHANGED on failure — a groom carrying a level that does not
        /// validate is a groom that will index out of bounds in the renderer.
        [[nodiscard]] static bool AttachLodLevels(GroomAsset& groom, std::vector<GroomLodLevel> levels,
                                                  std::string& outReason);
    };
} // namespace OloEngine
