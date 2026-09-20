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
// (0.543 against the kept member's 0.957). A kept strand is a SAMPLE of the
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
        /// centreline's 0.072-0.135, and it costs FEWER segments on a short
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

    // How a card level is formed. Every field is an AUTHORING choice with a
    // visible consequence, which is why none of them is a magic number inside
    // the builder.
    struct GroomCardSettings
    {
        /// Which of the two constructions above. The default is the one the
        /// measured comparison selected — see
        /// docs/analysis/groom-representation-lod-1252.md.
        GroomCardAggregation Aggregation = GroomCardAggregation::RepresentativeStrand;

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

        /// Recorded on the level as the apparent size the cook aimed at.
        /// Advisory: GroomLodPolicy decides where the hand-over happens.
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
        /// Summed cooked width of every member, and of every card, sampled at
        /// the parameters the emitted card uses. EQUAL to within float
        /// accumulation and the format's width clamp, and asserted rather than
        /// assumed: this IS the apparent-density claim of criterion 1, and a
        /// clustering bug that dropped a member would show here first.
        ///
        /// The two are NOT comparable between aggregations: MeanCentreline
        /// samples PointsPerCard uniform parameters and RepresentativeStrand
        /// samples its member's own points, so the same groom reports different
        /// totals under the two. Compare each against its own card sum, never
        /// one mode's against the other's.
        f64 MemberWidthSum = 0.0;
        f64 CardWidthSum = 0.0;
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
