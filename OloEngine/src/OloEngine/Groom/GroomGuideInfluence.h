#pragma once

// =============================================================================
// GroomGuideInfluence.h — how a rendered strand learns what its guides did.
// Issue #1250.
//
// THE SPLIT THIS FILE IS ONE HALF OF. GroomGuideSimulation.h moves a few hundred
// guides. This file is what turns that into a moving coat of two hundred
// thousand strands, and the two are separate because they have different cost
// curves: the solver is O(guides) per SUBSTEP, the interpolation is O(rendered
// points) per FRAME, and folding them together would run the expensive one at
// the cheap one's rate.
//
// WHAT IS INTERPOLATED, AND WHY IT IS A DISPLACEMENT. Not the guide's position —
// a strand is not at its guide. What travels is the guide's OFFSET FROM ITS OWN
// GROOMED REST SHAPE, sampled along the strand's normalised length, and added to
// the strand's own groomed rest shape. Three consequences, and the third is the
// one that decides it:
//
//   * A strand whose guides are undisturbed is at its rest position EXACTLY, so
//     a coat with the solver disabled is byte-for-byte the #1251 coat.
//   * Strands of different lengths in one group share guides without the short
//     ones being dragged to the long one's tip: the sample is by PARAMETER,
//     not by index.
//   * The interpolation never needs the guide's rest shape at runtime. Only the
//     displacement crosses the boundary, so the renderer cannot accidentally
//     mix a guide's curl into a strand that was groomed differently.
//
// WHAT THE TABLE IS KEYED ON. The GROUP, and the distance between REST ROOT
// POSITIONS — the cooked asset's first control points in groom object space, not
// the root UVs. Both are invariant under body deformation, because the search
// runs once over the asset and never sees a pose: the same reason GroomCoat.h
// keys on the root UV, reached through a different quantity. A strand is
// influenced only by guides in its OWN group: a whisker must not be blended
// toward the undercoat, which is what a purely geometric nearest-neighbour
// search on a muzzle would do.
//
// WHY THE TABLE IS DERIVED AT RUNTIME AND NOT COOKED. The .ologroom's minimum
// supported version equals its current version by design (GroomBinaryFormat.h):
// adding a cooked section would refuse every groom on disk today. The table is a
// pure function of the asset, it is built once per asset and cached, and the
// build is a few hundred milliseconds on the largest legal groom — against a
// re-cook of every groom in the project. Stated as a decision rather than left
// as an omission, because the next reader's question is "why is this not in the
// file?".
// =============================================================================

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    class GroomAsset;

    /// Guides that may influence one strand. Four is the standard choice and it
    /// is a LAYOUT constant, not a tunable: the table is a flat array of these,
    /// and the number is what makes a strand's influence a fixed-stride lookup
    /// instead of a second offset table.
    constexpr u32 GroomGuideInfluenceCount = 4;

    /// The sentinel for an unused slot. A strand with no guide in its group has
    /// four of them and is left at its groomed rest shape — which is a coat
    /// that visibly does not move, and therefore diagnosable, rather than one
    /// that moves with somebody else's guides.
    constexpr u32 GroomNoGuide = 0xFFFFFFFFu;

    /// One strand's guides. 32 bytes, no holes, trivially copyable — it is
    /// built in bulk and read in the innermost loop of the strand build.
    struct GroomGuideWeights
    {
        /// Indices into GroomGuideInfluenceTable::GuideCurves, NOT curve
        /// indices. The indirection is what lets a BUDGET drop guides without
        /// rebuilding the table: a strand keeps pointing at guide slot 7, and
        /// slot 7 is either simulated this frame or it is not.
        u32 Guides[GroomGuideInfluenceCount]{ GroomNoGuide, GroomNoGuide, GroomNoGuide, GroomNoGuide };
        /// Sums to 1 over the used slots, or is all-zero when there are none.
        f32 Weights[GroomGuideInfluenceCount]{ 0.0f, 0.0f, 0.0f, 0.0f };

        [[nodiscard]] bool operator==(const GroomGuideWeights&) const = default;
    };

    static_assert(sizeof(GroomGuideWeights) == 32, "GroomGuideWeights is read per strand in the build's inner loop");

    /**
     * @brief Which guides drive which strands, for one groom asset.
     *
     * Pure function of the asset. Built once, cached by asset handle, and
     * invalidated by the handle's asset identity — never by a frame.
     *
     * RefCounted because the cache hands it out by Ref and the strand build
     * holds a span into it across a frame; a vector by value would be a
     * megabyte-scale copy per groom per frame.
     */
    class GroomGuideInfluenceTable : public RefCounted
    {
      public:
        /// One entry per CURVE of the groom, in curve order, so an index into
        /// it is the curve index and there is no second mapping to get wrong.
        [[nodiscard]] const std::vector<GroomGuideWeights>& GetWeights() const noexcept
        {
            return m_Weights;
        }

        /// The CURVE index of each guide slot. Its size is the guide count and
        /// it is sorted ascending, so a budget that takes a stride over it
        /// takes a spatially even subset rather than one end of the pelt.
        [[nodiscard]] const std::vector<u32>& GetGuideCurves() const noexcept
        {
            return m_GuideCurves;
        }

        [[nodiscard]] u32 GetGuideCount() const noexcept
        {
            return static_cast<u32>(m_GuideCurves.size());
        }

        /// Strands that found no guide in their own group. Non-zero is a fact
        /// about the AUTHORING — a group was groomed with no guide curve — and
        /// is surfaced so a coat that does not move has an answer in the editor
        /// rather than a bug report.
        [[nodiscard]] u32 GetUnguidedStrands() const noexcept
        {
            return m_UnguidedStrands;
        }

        [[nodiscard]] u64 GetCpuMemoryBytes() const noexcept;

      private:
        friend Ref<GroomGuideInfluenceTable> BuildGroomGuideInfluence(const GroomAsset&);

        std::vector<GroomGuideWeights> m_Weights;
        std::vector<u32> m_GuideCurves;
        u32 m_UnguidedStrands = 0;
    };

    /**
     * @brief Build the influence table for `groom`.
     *
     * Never null. A groom with no guides produces a table whose guide count is
     * zero and whose every strand is unguided, which is the correct description
     * of a coat that cannot be simulated — not an error, and not a null the
     * caller has to branch on.
     *
     * A GUIDE IS ITS OWN SOLE INFLUENCE, with weight 1. Blending a guide toward
     * its neighbours would make the rendered guide disagree with the particle
     * the solver actually moved, so the debug view and the coat would show
     * different curves and only one of them would be the simulation.
     */
    [[nodiscard]] Ref<GroomGuideInfluenceTable> BuildGroomGuideInfluence(const GroomAsset& groom);

    /**
     * @brief The per-frame displacement of every simulated guide, by parameter.
     *
     * Object space, and laid out by `GuideOffsets` — the SAME prefix table the
     * solver state carries, handed straight through rather than re-derived.
     *
     * `Displacements` is this frame's offset from the groomed rest shape and
     * `PrevDisplacements` is last frame's. The pair is what makes a simulated
     * coat's motion vectors real: the previous position of a moving strand is
     * not recoverable from any matrix, exactly as #1249 found for the binding.
     * An empty `PrevDisplacements` means "the same as current", which emits
     * exactly zero motion — the state a re-seeded frame is in.
     */
    struct GroomGuideDisplacements
    {
        std::span<const u32> GuideOffsets{};
        std::span<const glm::vec3> Displacements{};
        std::span<const glm::vec3> PrevDisplacements{};

        /// Which guide SLOT of the influence table each simulated guide is, so
        /// a budgeted subset can be looked up by the slot the table names.
        /// Size equals the guide count; GroomNoGuide is not a legal entry.
        std::span<const u32> SlotOfGuide{};

        [[nodiscard]] u32 GuideCount() const noexcept
        {
            return GuideOffsets.empty() ? 0u : static_cast<u32>(GuideOffsets.size() - 1u);
        }

        [[nodiscard]] bool IsUsable() const noexcept
        {
            return GuideCount() > 0u && Displacements.size() == GuideOffsets.back() &&
                   SlotOfGuide.size() == GuideCount() &&
                   (PrevDisplacements.empty() || PrevDisplacements.size() == Displacements.size());
        }
    };

    class GroomGuideStandInSet;

    /**
     * @brief Everything the strand build needs to move a rendered strand.
     *
     * Passed by pointer and null for an un-simulated groom, so that path is
     * byte-for-byte the one that existed before this issue — the same shape
     * GroomStrandDeformation and GroomCoatContext already use.
     */
    struct GroomStrandSimulation
    {
        const GroomGuideInfluenceTable* Influence = nullptr;
        GroomGuideDisplacements Displacements{};

        /// Slot -> index into the solver's guide dimension, or GroomNoGuide for
        /// a slot the budget did not simulate this frame. Sized by the table's
        /// guide count, so a strand's slot lookup is always in range.
        std::span<const u32> GuideOfSlot{};

        /// Every guide slot's stand-ins (#1509), published beside the simulated
        /// guides rather than expanded into them (#1589): the coat is drawn
        /// from EVERY slot's displacement, a left-out slot's being the blend of
        /// its stand-ins. Null when every slot was simulated -- the simulated
        /// set is then already every slot. Read only through ExpandsStandIns().
        /// A CPU reader samples the expansion (GroomGuideExpansion), never the
        /// simulated subset as it stands: that would leave each left-out slot
        /// at rest.
        const GroomGuideStandInSet* StandIns = nullptr;

        [[nodiscard]] bool IsUsable(u32 curveCount) const noexcept
        {
            return Influence != nullptr && Influence->GetWeights().size() == curveCount &&
                   GuideOfSlot.size() == Influence->GetGuideCount() && Displacements.IsUsable();
        }

        /// Whether this frame is drawn from its stand-ins' expansion: there are
        /// stand-ins, one per slot of the table, and they describe these
        /// displacements (GroomGuideStandInsDescribe). False draws the simulated
        /// set as it is, which is also what stand-ins that do not describe it
        /// get -- the rule the producer applied when it expanded them itself.
        [[nodiscard]] bool ExpandsStandIns() const noexcept;
    };

    /**
     * @brief The interpolated displacement for one point of one strand.
     *
     * @param simulation the frame's guide displacements and the asset's table.
     * @param curveIndex the strand.
     * @param t root-to-tip parameter in [0,1]. Sampled along the GUIDE by this
     *        parameter, so a short strand and a long one sharing a guide both
     *        get the part of its motion that corresponds to their own height.
     * @param previous read the previous frame's displacements instead.
     *
     * Returns the zero vector for an unguided strand, for a strand whose guides
     * were all outside this frame's budget, and for a `t` that is not finite —
     * i.e. the strand stays at its groomed rest shape, which is the
     * diagnosable answer rather than the plausible one.
     */
    [[nodiscard]] glm::vec3 SampleGroomGuideDisplacement(const GroomStrandSimulation& simulation, u32 curveIndex,
                                                         f32 t, bool previous) noexcept;

    /**
     * @brief Whether `curveIndex` has any guide this frame's budget simulated.
     *
     * The predicate the strand build branches on, and a predicate rather than a
     * comparison of the sampled displacement against zero: a guide that is being
     * simulated and happens to be exactly at rest this frame is SIMULATED, and
     * an equality test on a float would call it unguided — which is also the
     * comparison the coding rules forbid outright.
     */
    [[nodiscard]] bool HasGroomGuideInfluence(const GroomStrandSimulation& simulation, u32 curveIndex) noexcept;

    // ── Guide stand-ins (issue #1509) ──────────────────────────────────────────
    //
    // A simulation budget simulates a SUBSET of the guide slots. A slot left out
    // used to have no displacement at all, and a strand whose four guides were
    // all left out -- a third of the coat at a quarter of the guides -- was
    // drawn at its GROOMED REST shape while the coat around it draped. A guide
    // curve's own influence is itself alone, so every left-out guide curve did
    // the same. The stand-ins give every slot a displacement: a left-out slot
    // takes the blend of the nearest simulated guides of its own group.

    /**
     * @brief For every guide slot, the simulated guides that stand in for it.
     *
     * `Guides[k]` holds a SIMULATED GUIDE INDEX (the index `guideOfSlot` maps
     * to), not a slot. A simulated slot stands in for itself with weight 1. A
     * left-out slot takes up to GroomGuideInfluenceCount nearest simulated
     * guides by root distance, from its own group when that group has any, else
     * from its own ROLE, weighted by inverse distance and normalised. Never from
     * another role: a role with no simulated guide -- an authored budget of zero,
     * "an undercoat that never leaves the skin" -- keeps no stand-in and stays at
     * rest. A budget that simulates nothing leaves every entry empty.
     *
     * @param slotRoots    each slot's root point, any one space
     * @param slotGroups   each slot's curve group id
     * @param slotRoles    each slot's coat role
     * @param guideOfSlot  slot -> simulated guide index, or GroomNoGuide
     */
    void BuildGroomGuideStandIns(std::span<const glm::vec3> slotRoots, std::span<const u32> slotGroups,
                                 std::span<const u32> slotRoles, std::span<const u32> guideOfSlot,
                                 TArray<GroomGuideWeights>& outStandIns);

    /**
     * @brief Every slot's displacement, from the simulated guides' ones.
     *
     * Lays out one entry per SLOT, in slot order, `slotPointCounts[s]` points
     * each: a simulated slot copies its guide's points, a left-out slot blends
     * its stand-ins' displacements sampled BY PARAMETER along the guide (the
     * rule SampleGroomGuideDisplacement follows). Returns false, leaving the
     * outputs empty, for inputs that do not describe one layout.
     */
    [[nodiscard]] bool ExpandGroomGuideDisplacements(std::span<const GroomGuideWeights> standIns,
                                                     std::span<const u32> slotPointCounts,
                                                     std::span<const u32> guideOffsets,
                                                     std::span<const glm::vec3> displacements,
                                                     TArray<u32>& outOffsets, TArray<glm::vec3>& outDisplacements);

    /**
     * @brief Whether one expansion is described: the check
     * ExpandGroomGuideDisplacements makes before it writes anything.
     *
     * One point count per stand-in; offsets that start at 0, never decrease and
     * end inside `displacementCount`; every stand-in naming a guide the offsets
     * have. GroomExpandGuides.comp trusts the layout it is handed, so the CPU
     * asks this before it packs one for the kernel.
     */
    [[nodiscard]] bool GroomGuideStandInsDescribe(std::span<const GroomGuideWeights> standIns,
                                                  std::span<const u32> slotPointCounts,
                                                  std::span<const u32> guideOffsets, sizet displacementCount) noexcept;

    /**
     * @brief Every guide slot's stand-ins for one simulation budget (#1589).
     *
     * Built when the budget changes which guides are simulated, then never
     * written again, so the strand requests of later frames share it by Ref and
     * a GPU buffer that sent it can tell it is unchanged by its serial alone:
     * the stand-ins cross the bus once per budget step, and only the simulated
     * guides' points cross every frame. The dense layout is precomputed here
     * too -- each slot's window is the running sum of the slots' point counts.
     */
    class GroomGuideStandInSet : public RefCounted
    {
      public:
        /// Null when the two arrays differ in length, are empty, or the slots'
        /// points do not fit a u32 offset.
        [[nodiscard]] static Ref<GroomGuideStandInSet> Create(TArray<GroomGuideWeights> standIns,
                                                              TArray<u32> slotPointCounts);

        /// Per slot, its stand-ins: SIMULATED GUIDE indices and their weights.
        [[nodiscard]] std::span<const GroomGuideWeights> GetStandIns() const noexcept
        {
            return { m_StandIns.GetData(), static_cast<sizet>(m_StandIns.Num()) };
        }
        /// Per slot, its curve's point count: the length of its window.
        [[nodiscard]] std::span<const u32> GetSlotPointCounts() const noexcept
        {
            return { m_SlotPointCounts.GetData(), static_cast<sizet>(m_SlotPointCounts.Num()) };
        }
        /// Slots + 1 entries: where each slot's window starts in the dense
        /// layout, and the total.
        [[nodiscard]] std::span<const u32> GetSlotOffsets() const noexcept
        {
            return { m_SlotOffsets.GetData(), static_cast<sizet>(m_SlotOffsets.Num()) };
        }
        [[nodiscard]] u32 GetSlotCount() const noexcept
        {
            return static_cast<u32>(m_StandIns.Num());
        }
        /// Every slot's points: the dense layout's size.
        [[nodiscard]] u32 GetPointCount() const noexcept
        {
            return m_SlotOffsets.IsEmpty() ? 0u : m_SlotOffsets.Last();
        }
        /// Unique per set for the life of the process, so "is this the set I
        /// sent?" never mistakes a new set at a reused address for the old one.
        [[nodiscard]] u64 GetSerial() const noexcept
        {
            return m_Serial;
        }
        /// One past the highest simulated guide any stand-in names (0 when
        /// none does), found once at Create: the set never changes, so whether
        /// it fits a frame's guides is one comparison, not a walk of every slot
        /// for every reader of every frame.
        [[nodiscard]] u32 GetGuidesNamed() const noexcept
        {
            return m_GuidesNamed;
        }
        /// Whether these stand-ins describe a frame of guides laid out by
        /// `guideOffsets` over `displacementCount` points: GroomGuideStandInsDescribe's
        /// answer, from the guides named at Create rather than a walk.
        [[nodiscard]] bool Describes(std::span<const u32> guideOffsets, sizet displacementCount) const noexcept;
        [[nodiscard]] u64 GetCpuMemoryBytes() const noexcept;

      private:
        TArray<GroomGuideWeights> m_StandIns;
        TArray<u32> m_SlotPointCounts;
        TArray<u32> m_SlotOffsets;
        u64 m_Serial = 0;
        u32 m_GuidesNamed = 0;
    };

    /**
     * @brief The dense simulation a CPU reader samples (#1589).
     *
     * The producer publishes the simulated guides and their stand-ins, not
     * their expansion; the GPU expands them where it draws the coat
     * (GroomExpandGuides.comp). A CPU reader -- the CPU deformation path, the
     * caster padding, the coat bake's drawn pose, the ray-traced proxy --
     * samples the same expansion here, through ExpandGroomGuideDisplacements:
     * one entry per SLOT in slot order, every slot mapped to itself. Exactly
     * what the producer used to publish, so every CPU reader reads what it
     * read before.
     *
     * Owns its arrays; the view Expand returns is valid until the next call.
     */
    class GroomGuideExpansion
    {
      public:
        /// `simulation` itself when it does not expand stand-ins. Otherwise its
        /// expansion -- and, with `withPrevious`, last frame's, all or nothing:
        /// a previous frame that does not expand is no previous frame (zero
        /// motion), never a partial one. A current frame that does not expand
        /// returns the simulated set as it stands.
        [[nodiscard]] GroomStrandSimulation Expand(const GroomStrandSimulation& simulation, bool withPrevious = true);

        [[nodiscard]] u64 GetCpuBytes() const noexcept;

      private:
        TArray<u32> m_Offsets;
        TArray<u32> m_PrevOffsets;
        TArray<u32> m_IdentitySlots;
        TArray<glm::vec3> m_Displacements;
        TArray<glm::vec3> m_PrevDisplacements;
    };
} // namespace OloEngine
