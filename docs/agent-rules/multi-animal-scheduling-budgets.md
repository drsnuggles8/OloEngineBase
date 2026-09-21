# Multi-animal scheduling: the population budget, and what it may never buy back (#1258)

Read before touching `OloEngine/src/OloEngine/Scene/AnimalScheduler.{h,cpp}`,
`AnimalBudgetComponent` / `AnimalPathComponent`, `Scene::GatherAnimalWorkItems`, the
`RendererSettings::Animal*` knobs, or `Core/FrameTimeTail.{h,cpp}`.

A per-entity LOD ladder is right about one entity and blind to the herd. This file is the arbiter
that sits above every ladder, and the rules are the ones that keep it from fixing a frame time by
quietly breaking the picture.

## The rules

1. **The budget is spent in MODEL UNITS, never in a live clock reading.** A scheduler that
   reads the clock allocates differently on a busy machine than on an idle one, so the population's
   trajectories stop reproducing and every capture downstream is noise — and reproducibility is
   criterion 1. But `AnimalCostModel`'s coefficients are **structural ratios, not a wall-clock
   calibration** — a unit is not a microsecond, and
   [multi-animal-scheduling-budgets-1258.md §4](../analysis/multi-animal-scheduling-budgets-1258.md)
   says so at length. **Any conclusion drawn from them must carry a margin**: how wrong the model
   may be before the conclusion flips, the way §1's draw-call finding does (20.4×). A number out of
   this model quoted without one is not evidence.

2. **Four axes, four allowances — never one "animal quality" scalar.** Deformation, Simulation,
   Visibility and Shadow degrade differently and are noticed differently: thinned guides look
   identical until the animal moves, thinned strands are visible standing still, a quarter-rate
   skeleton is invisible at distance and judder up close. `AnimalSchedulingCensus.
   TheDominantAxisMovesWithApparentSizeSoOneQualityScalarCannotBeRight` is the measurement that
   settles it — the dominant axis at 16 px is not the dominant axis at 720 px, so a scalar right for
   the herd is wrong for the hero.

3. **One pool makes the axes compete, and the winner is a tuning accident.** The allowance splits by
   normalised weight *per axis*, so an expensive simulation cannot eat the allowance that keeps the
   population's silhouette. Weights are normalised rather than clamped: `2/1/1/1` and
   `0.4/0.2/0.2/0.2` are the same policy, and no author can hand out 140 % of a frame.

4. **A reduced tick rate is bounded in SCREEN SPACE, not by a distance.** An animal ticked at
   `1/2^k` moves `2^k` times as far between poses; that is unnoticeable exactly while the
   displacement stays sub-pixel and is a visible stutter as soon as it does not.
   `MaxDeformationStepForPoseBound` derives the cap from the animal's own authored motion and its
   apparent size. A distance threshold would be right for a grazing animal and wrong for a sprinting
   one at the same range.

5. **`AnimalBudgetComponent::m_FullRateMotionMetres` is AUTHORED, and it must stay authored.**
   Measuring it needs the pose evaluated, which is the work the deformation budget exists to skip —
   so a measured value would exist only for animals nobody was budgeting. It is also a property of
   the *clip set*: a sprint cycle is the bound whether or not the animal is sprinting now, and
   budgeting on the current gait lets an idle animal earn a rate it cannot keep when it starts to
   run.

6. **One halving per hold window, on every axis.** Applying a three-step coarsening in one frame is
   a factor-of-eight change in update rate between consecutive frames, which *is* the "abrupt motion
   change" the issue names. The request is rate-limited to `current + 1` *before* the hold sees it;
   the next window takes the next step. Refining stays immediate, for `GroomLod::ApplyHold`'s reason.

7. **`MinVisibleStrands` is a floor the budget may not cross, and the DISTANCE LADDER may not cross
   it either.** The cap is applied as a `min()` against the *desired* step, not only against the
   allocated one — "invisible distant coats" is a failure the per-entity ladder produces unaided,
   with no budget pressure involved at all. A groom authored *below* the floor is not forced up to
   it: the floor stops the budget thinning a coat to nothing, it is not a minimum the asset must
   meet, and treating it as one silently refuses to LOD a deliberately sparse groom.

8. **Starvation is prevented by the SERVICE ORDER, and the bound is RELATIVE.** The per-axis
   starvation counter is the second sort key, so an animal passed over rises until it outranks its
   competitors; `StarvationFrames` then removes it from the candidate set entirely while any
   same-role peer is still eligible. A counter that were merely reported would be a diagnostic; one
   that is part of the sort, and then of the eligibility test, is a mechanism.
   **An ABSOLUTE bound — "nobody is below desired for more than N frames" — is unachievable** at any
   budget small enough to matter, because when the whole population must be coarsened there is
   nobody to swap with; the first version of the contract test asserted it and failed at 200 frames
   against a correct scheduler. For the same reason the rule is *dropped* for a pass in which every
   candidate is over the bound, or the axis would stay over budget forever.

9. **Role groups are exhausted in turn, never interleaved.** Every Background animal must be at its
   cap before a Featured one gives way. That is "preserve hero quality" made operational: a herd
   that thins while the hero stays sharp, rather than a population that degrades uniformly and takes
   the hero down with it.

10. **An unservable budget is REPORTED, never absorbed by the hero.** With `AnimalProtectHero` set
    the hero is not a candidate at all; when everyone else is at their cap and the frame still does
    not fit, `AnimalSchedulerStats::BudgetExceeded` fires. Degrading the hero to make the number fit
    hides the one thing the budget exists to protect, and the frame merely looks slightly wrong with
    every counter green.

11. **The service order's last tie-break is the UUID, and it is load-bearing.** Without it two
    identical animals tie and the allocation falls back to the gather order — which is EnTT's
    iteration order, a function of allocation history rather than of the scene. Loading the same
    scene twice would then schedule it differently.

12. **A NaN apparent size is repaired before the comparator sees it.** `<` is false in both
    directions for a NaN, which is not a strict weak ordering: `std::sort` on it is undefined
    behaviour, not a wrong answer. `ServiceOrder` holds a sanitised copy rather than reading through
    to the item.

13. **The scheduler state lives in `Scene`, keyed by UUID — never in a pass.** A pass runs once per
    CAMERA and the hysteresis and starvation counters must advance once per FRAME. In a pass, a
    split-screen scene burns its hold twice as fast and starves at double rate against a budget that
    did not. Same rule, same fix, as `GroomLodState` and the guide simulation's clock.

14. **Turning the budget off RESETS the state.** Off is the A/B control arm for every capture, so it
    has to be the same picture every time rather than whichever allocation the population last left
    behind.

15. **`AnimalPathComponent` is CLOSED FORM in elapsed time, never an accumulation.** An accumulated
    path drifts with the frame rate, so the same scene captured at a different `dt` puts the animals
    somewhere else and every measurement compares two different populations. Same rule, same reason,
    as `BenchmarkManifest::CameraPoseAtFrame`. The facing comes from the analytic *derivative*, not
    a finite difference — a difference is undefined at `dt == 0` (a paused frame) and noisy at small
    `dt`, which reads as an animal spinning while standing still.

16. **The path is a Lissajous figure and not a circle, deliberately.** A herd on circles holds every
    animal at a constant distance from the camera, so the distance ladders never move and the budget
    is never exercised. The population looks busy and measures nothing.

17. **Report the TAIL, not the mean.** Amortising work across frames does not reduce it, it moves
    it; done badly — every animal's expensive frame landing on the same frame — the mean falls and
    the 99th percentile gets *worse*. `FrameTimeTail` reports p50/p95/p99/max and a count of
    over-budget frames, because at a 120-sample window p99 is one frame and the percentile alone
    cannot tell one bad frame from six.

18. **`FrameTimeTail` uses NEAREST-RANK percentiles, matching `perf_trend.py`.** Two percentile
    conventions differing by an interpolation is an afternoon of argument about a one-frame
    discrepancy between an in-editor number and a CI trend number.

19. **The minimum-of-N the perf suite uses is the OPPOSITE measurement, and both are right.** A
    microbenchmark wants the machine's best effort with scheduler noise removed, so it takes the
    minimum. A frame-pacing question wants exactly that noise, so it takes the tail. Using either
    for the other's question is how a stutter ships green.

20. **Count what the budget CANNOT reach, and keep it in the suite.** Draw calls are priced in
    `AnimalCostModel` on purpose: criterion 2 is an instruction not to *assume* they are the limit,
    and a model that omitted them could not have been wrong about them. They are subtracted off the
    top so the axis allowances are honest — if they dominated, every allowance would be zero and
    `BudgetExceeded` would fire on frame one. The census ships as a test, per
    [geometry-lod-measure-the-unreachable-cost.md](geometry-lod-measure-the-unreachable-cost.md)
    rule 10.

## What a wrong schedule looks like, and which counter names it

| Symptom | Counter that says so |
|---|---|
| The herd never coarsens however large it gets | no animal carries an `AnimalBudgetComponent`; `AnimalsConsidered` is 0 |
| The hero is soft in a crowded shot | `CoarsenedByRole[Hero]` non-zero — `AnimalProtectHero` is off |
| The frame is over budget and nothing is being cut | `BudgetExceeded` with `AnimalsCapHeld` high: the population has outgrown the caps, not the budget |
| Distant animals are bald | `AnimalsAtVisibilityFloor` is 0 while coats vanish — the floor is not reaching `MaxStep` |
| One animal is permanently coarse while its neighbours are sharp | NOT `MaxStarvedFrames` — it measures PRESSURE and grows for everybody when nothing can be served. The unfairness condition is relative: a same-role peer sitting AT its desired step on that axis while this one is below it. Copy `AnimalSchedulerStarvation.NoAnimalIsPassedOverWhileAPeerSitsAtItsDesiredStep` |
| The population flickers between quality levels | `StepChanges` staying near `AnimalsConsidered * 4` |
| A distant animal judders | `AnimalsAtPoseStepCap` is 0 — `m_FullRateMotionMetres` is authored too low for its fastest clip |
| Frame time improved but it feels worse | read `FrameTimeTail::Query().P99Ms`, not the mean — see rule 17 |
| The same scene schedules differently on two runs | something read a clock or the gather order — rules 1 and 11 |

## Adding a field

- New `AnimalBudgetPolicy` / `AnimalScheduleState` / `AnimalSchedule` field → it is compared with a
  whole-object `memcmp`, so order it 4-byte-then-1-byte with the tail named explicitly and update
  the `static_assert` on `sizeof`. All five value types are listed in `BitwiseEqualLayoutTest`,
  which is the mechanism — `std::has_unique_object_representations_v` is false for any type holding
  a float and cannot be the guard.
- New `AnimalWorkAxis` → `AnimalWorkAxisCount` sizes five arrays and the census prints four columns.
  `AxisUnitWork` and `AxisCoefficient` both switch on it *without* a default case on purpose, so a
  new axis is a compile error in both rather than a silently free one.
- New component field → the cross-binding walk in `CLAUDE.md`. Both components are all-trivial and
  all-public, so the `AllComponents` tuple, the `OnComponent*` no-ops, scene YAML and the MCP field
  registry are generated; the save-game `Serialize` overload plus `RegisterAll`, and the editor
  inspector, are hand-written. Lua is deliberately skipped, matching `GroomComponent` and
  `GroomLodComponent` — say so rather than leaving it as an omission.

## Where the numbers are

`AnimalSchedulingCensusTest` prints the cost breakdown and is the source of the analysis document's
tables. Re-run it rather than trusting the tables if you change a coefficient:

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=AnimalSchedulingCensus.*
```
