# Groom representation LOD: the ladder, the budgets and the compensation (#1252)

Read before touching `OloEngine/src/OloEngine/Groom/GroomLod*.{h,cpp}`,
`GroomStrandMesh.{h,cpp}`'s `GroomBuildSource`, `Scene::PublishGroomStrandRequests`'s LOD block,
`GroomRenderPass`'s width compensation, or section 10 of the `.ologroom` format.

## The rules

1. **The coverage compensation is `1/k`, LINEAR — never `1/sqrt(k)`.** A strand is a *band*: the
   area it covers is its projected length times its projected width, so a coat's covered area goes
   as `count x width`. Halving the count and doubling the width restores it exactly.
   `FoliageLod::CoverageCompensation` uses `1/sqrt(k)` because a foliage instance is a *sprite*
   whose area goes as the square of its linear size; borrowing that factor here under-compensates a
   sixteenth-density coat by 4x. Measured: uncompensated, a half-density coat carries 0.63–0.64 of
   the area and an eighth-density one 0.19–0.21; compensated, every arm lands in 0.98–1.06.

2. **Compensate on the ACHIEVED fraction, never the requested one.** The strand budget is spent as
   an integer *stride per role* (`GroomStrandMesh.cpp`), so a budget that asked for 0.4 of a role
   retains a third of it. The pass reads `StrandsSelected / StrandsAvailable` back out of the cache
   entry's build stats; compensating on the policy's number instead leaves the coat a sixth thin at
   one step and compounds at every step after.

3. **Budgets move in HALVINGS; the compensation is continuous.** The strand geometry is cached,
   keyed on the build settings, so a budget that slid with the camera would rebuild every groom's
   vertex buffer every frame — the exact cost the cache exists to remove. The width compensation is
   a UBO value, so at the instant a stride doubles it doubles with it and the coat's total coverage
   does not move. What remains at a step is *spatial*, and that is what the distance thresholds
   bound.

4. **A card is a KEPT STRAND widened to its cluster's total width — never an average.** A mean
   centreline is the textbook hair card and it measured worse on every coat at every distance:
   averaging curves that diverge produces a shorter, straighter curve, so it loses exactly the
   spread that gives a tuft its silhouette. On the short coat it lost 45 % of the covered area
   (0.548 against the kept member's 0.963). `GroomCardAggregation::MeanCentreline` survives as the
   measured-and-rejected alternative, the way `GroomCompositionMode::AlphaToCoverage` does.

5. **The card tier earns its cook by being ABOVE the compensation cap, not by being cheaper.** The
   runtime stride can thin by `k` and widen by `1/k` for free, and at modest reductions it ties the
   card within noise. But the runtime widening is capped at 8x, because a strand widened sixty
   times is a flat band rather than a fibre — so past the cap a stride *cannot* restore the density.
   At a 59x reduction the matched strand arm carries **0.23** of the coat's area and the card
   carries **1.01**. That gap is the whole reason a level is cooked rather than derived. A cell that
   only reduces by 4–9x is not worth cooking, and the builder's "no reduction" refusal is the floor
   rather than the target.

   The card's SILHOUETTE advantage is a separate, weaker claim and is asserted as two: it is never
   materially worse (5 % tolerance) anywhere in the card band, and it wins outright at and below
   64 px, where the margin is a factor (2.8x on the scalp) rather than the 1.3 % the short coat
   scores at the top of the band. A 1.3 % gap between two independently summed measurements is a
   coincidence, not a contract.

6. **A card is a cooked CURVE, so every tier goes through one shader.** Criterion 1 asks for colour
   and highlight response to be preserved across a transition; the cheapest way to be sure of that
   is for there to be only one shading path. The fibre BCSDF (#1247), the coat shadow volume
   (#1248), the per-strand tint (#1251) and the guide simulation (#1250) all keep working at card
   range without a second implementation that could disagree with the first. A baked card *texture*
   would fix the coat's shading at bake time and is deliberately not what this is.

7. **A LOD level carries a SOURCE MAP back to the base groom, and it is not optional.** The
   binding's rest frame and the guide influence table are both indexed by base curve. Without the
   map a coat at card range stops following the body and stops moving — a silent detachment at
   exactly the distance nobody is looking closely. `Scene` remaps the selected curves through it
   before evaluating root transforms, and `GroomCooker::Canonicalize` remaps it when it reorders.

8. **Root UVs must be inside ±16, or clustering is meaningless.** `GroomCoatClumpCell` *clamps* a
   UV to ±16 before quantising, so every strand past the clamp lands in one cell. A groom whose
   chart runs further cooks a handful of enormous cards that carry a fraction of its density and
   looks like a working level. `GroomLodBuilder` refuses such a groom **by name**; #1251's clumping
   has the same limitation silently. This was found by measurement, not inspection — a fixture whose
   root U reached 11 459 cooked 64 cards for a 20 000-strand scalp carrying 16 % of its coverage.

9. **Two anti-thrash mechanisms, two different bounds. State both.** The threshold *slide*
   (`Hysteresis`) makes a dead band `2h x threshold` wide, so a camera whose travel is narrower than
   that band causes **at most one** transition ever, whatever its period. The *frame hold*
   (`HoldFrames`) bounds the other case: a coarsening needs that many consecutive stable frames, so
   a camera alternating every frame never coarsens at all. Refining is always immediate — a coarse
   coat up close is a picture anyone can see, a fine one at distance is merely expensive.
   `FoliageLod` has only the first, because a plant carries no per-instance state; a groom is an
   entity and its LOD state persists in `Scene`.

10. **The LOD state lives in `Scene`, keyed by UUID — never in the pass.** A pass runs once per
   CAMERA; a hysteresis must advance once per FRAME. In the pass, a split-screen scene burns its
   hold twice as fast. Same reason, same fix, as the guide simulation's clock.

11. **The three budgets get three curves AND three stability counters.** Criterion 3 is that
   visibility, simulation and shadow scale down *independently*. Sharing one counter recouples them
   through the hysteresis even when the curves are separate: a shadow step that keeps flickering
   would pin the visibility step at its old value.

12. **The shadow axis is a BIAS, not a budget.** A shadow volume spends a resolution, not a count,
    so `GroomLodPolicy::Shadow`'s step is *added* to `GroomCoatShadow::SelectCoatLodStep`'s answer
    rather than replacing it. The two stay separately authorable.

13. **The cache key includes the representation.** A card level and the base groom are different
    curve sets; at the same budget they hash the same, so a coat that handed over would be served
    the strand geometry it had a moment ago and the hand-over would do nothing until something else
    evicted it.

14. **The shell ("mesh") tier is measured and NOT shipped.** A shell claims coverage 1 everywhere
    inside the silhouette, so it is only honest where the real coat has saturated. Across three
    reference coats and six distances down to 4 px, the highest *solid* fraction measured anywhere
    was **0.064**. A shell would replace a see-through coat with a solid lump.
    `GroomLodFallbackReason::MeshTierNotSelected` reports it, and
    `GroomLodShellTier.TheShellTierIsMeasuredBeforeItIsRefused` fails if a future coat ever
    saturates — which is the correct signal to revisit, not a reason to hard-code the refusal.

## `sizeof(GroomLodComponent)` and the other pinned layouts

`GroomLodComponent`, `GroomLodPolicy`, `GroomLodBudgetCurve`, `GroomLodDecision` and
`GroomLodState` all implement `operator==` as `Math::BitwiseEqual(*this, other)`, so every one of
them must have **no padding**: members ordered 4-byte-then-1-byte with the tail named explicitly
(issue #1019). All five are listed in `BitwiseEqualLayoutTest`, which is the mechanism —
`std::has_unique_object_representations_v` is false for any type holding a float and cannot be the
guard. A field added without reordering fails the `static_assert` on the size *and* that test.

## The version bump costs every cooked groom on disk

`.ologroom` is a *derived* artifact, so `MinSupportedVersion` moves with `CurrentVersion`
(`binary-format-versioning.md`). Version 3 added section 10, so **every `.ologroom` written before
this change must be re-imported**; the failure is a readable version error rather than a reader
mis-parsing section 9 as section 10, and `GroomLodRoundTripTest` pins that refusal. The committed
reference grooms under `OloEditor/assets/Grooms/` were regenerated with the change.

## What a wrong LOD looks like, and which counter names it

| Symptom | Counter that says so |
|---|---|
| The coat never leaves the strand tier | `GroomLodFallbackReason::LevelNotCooked` — re-import the groom |
| The coat is on cards where you expected a shell | `MeshTierNotSelected` — read the analysis, not the settings |
| The coat flickers between tiers | `GroomLodStats::RepresentationChanges` staying near the groom count |
| The coat is thinner than authored at range | `GroomsAtCompensationCap` non-zero; `MaxWidthCompensation` at the cap |
| The coat detaches from the body only past the hand-over | a stale source map — rule 7 |
| The hand-over rebuilds geometry every frame | the budget is sliding rather than halving — rule 3 |

## Where the numbers are

`docs/analysis/groom-representation-lod-1252.md` holds the measured comparison. Every number in it
is an assertion in `GroomLodComparisonTest`, so the argument behind the selected representation
fails loudly rather than ageing quietly — the same discipline
`docs/analysis/groom-strand-visibility-1246.md` and `groom-coat-self-shadowing-1248.md` use.
