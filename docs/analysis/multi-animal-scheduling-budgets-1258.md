# Multi-animal scheduling and budgets: the measurements (#1258)

Every number here is an assertion in a committed test, so the argument behind the design fails
loudly rather than ageing quietly — the discipline
[groom-representation-lod-1252.md](groom-representation-lod-1252.md) and
[groom-strand-visibility-1246.md](groom-strand-visibility-1246.md) use.

Re-run rather than trusting the tables:

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=AnimalSchedulingCensus.*
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=AnimalScheduler*
cd OloEditor; ..\build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=AnimalBudgetVisualEvidenceTest.*
```

**Hardware.** All figures below are from `OLE` — Windows 11, RTX 4090 (NVIDIA GeForce RTX
4090/PCIe/SSE2), clang-cl **Debug**, OpenGL. The structural census is hardware-independent (it is a
count priced through a model); the calibration in §4 is not, and is labelled accordingly.

---

## 1. Criterion 2: which costs dominate

The issue's instruction is *not to assume* draw calls are the limit. `AnimalSchedulingCensusTest`
prices a mixed-distance population — one close-up hero, four featured long coats, and a herd of
alternating short and long coats — at full rate through `AnimalCostModel`.

| population | total (units) | deform | simulation | visibility | shadow | **draw calls** | dominant |
|---|---:|---:|---:|---:|---:|---:|---|
| herd = 8 | 12 602 | 3.1 % | 44.1 % | 28.7 % | 22.2 % | **1.86 %** | Simulation |
| herd = 32 | 29 106 | 3.6 % | 45.7 % | 28.0 % | 20.6 % | **2.14 %** | Simulation |
| herd = 96 | 73 116 | 3.8 % | 46.4 % | 27.7 % | 19.8 % | **2.28 %** | Simulation |
| herd = 256 | 183 142 | 3.9 % | 46.7 % | 27.6 % | 19.5 % | **2.33 %** | Simulation |

**Draw calls carry 1.9–2.3 % of a full-rate animal frame at every tested size.** So scheduling is
the right lever here and batching is not the missing piece — which is the answer the criterion
asked for, and it could have come out the other way: `DrawCallsAreNotTheLimitAtAnyTestedPopulation`
fails above 5 %, and the correct response to that failure would be #1031's batching rather than
more scheduling.

**These are model-priced shares, so the conclusion carries its own error bar.** §4 is explicit that
the coefficients are structural ratios rather than a wall-clock regression, which would make "2.3 %"
on its own worth very little. What makes the conclusion usable is the MARGIN:
`TheDrawCallConclusionSurvivesALargeErrorInItsOwnCoefficient` measures how far `PerDrawCall` would
have to be wrong before draw calls became the largest line in the frame, and the answer is
**20.4× — 133 units per draw instead of 6.5**. A conclusion that survives a twentyfold error in its
own input is safe at the order-of-magnitude accuracy actually claimed; one that survived only a 1.5×
error would not be, and that assertion fails below 10× so the distinction is checked rather than
asserted.

The share *rises* with population (1.86 → 2.33 %) because draw calls are the one cost no step
scales, so they become a larger fraction of a herd that is otherwise being thinned. That is the
reason they are subtracted off the top before the axis allowances are computed.

## 2. Why four axes and not one quality scalar

The dominant axis is a function of apparent size, so a scalar that was right for the herd would be
wrong for the hero. Measured on the long-coated reference at its own ladder's step:

| apparent size | deform | simulation | visibility | shadow | dominant |
|---|---:|---:|---:|---:|---|
| 16 px | 9.0 % | 9.2 % | 6.0 % | **70.0 %** | Shadow |
| 48 px | 9.0 % | 9.2 % | 6.0 % | **70.0 %** | Shadow |
| 120 px | 7.8 % | 16.0 % | 10.4 % | **60.8 %** | Shadow |
| 320 px | 4.3 % | **35.6 %** | 23.3 % | 33.9 % | Simulation |
| 720 px | 2.7 % | **44.9 %** | 29.3 % | 21.3 % | Simulation |

The crossover sits between 120 px and 320 px. Close up the coat's *motion* dominates; at distance
the shadow volume does, because its resolution ladder bottoms out while the strand and guide counts
keep halving. `TheDominantAxisMovesWithApparentSizeSoOneQualityScalarCannotBeRight` fails if this
ever stops being true — which would be the signal that the four-axis design is no longer justified.

## 3. What the budget can and cannot reach

Before trusting a scheme that redistributes work, count the work it cannot reach
([geometry-lod-measure-the-unreachable-cost.md](../agent-rules/geometry-lod-measure-the-unreachable-cost.md)
rule 1). For the herd-of-96 population:

| | units | share |
|---|---:|---:|
| full rate | 73 116 | 100 % |
| irreducible (every axis at its cap, visibility at the strand floor, plus draw calls) | 6 292 | 8.61 % |
| **reachable by the budget** | 66 824 | **91.39 %** |

`TheBudgetReachesTheMajorityOfTheFrameButNeverAllOfIt` fails below 50 %, which would mean the caps
or the floors had grown to the point where scheduling could no longer be the primary lever.

## 4. The calibration — and its honest status

`AnimalCostModel`'s defaults are the coefficients the tables above are priced with:

| coefficient | value | unit |
|---|---:|---|
| `DeformationPerBone` | 0.42 | units per bone per pose evaluation |
| `SimulationPerGuidePoint` | 0.31 | units per guide particle per substep |
| `VisibilityPerStrand` | 0.0135 | units per strand built |
| `ShadowPerVoxel` | 0.0009 | units per coat-shadow voxel |
| `PerDrawCall` | 6.5 | units per draw |

**A UNIT IS A UNIT. It is not a microsecond, and this document previously said it was.** No
per-axis wall-clock calibration was performed, so attaching a time unit to these numbers claimed a
measurement that does not exist. What they are is a set of **structural ratios**: what one bone, one
guide particle, one strand, one voxel and one draw cost *relative to each other* as this engine
implements them. The budget is a number in the same arbitrary unit, which is why the tiers read
2 000 / 4 000 / 6 000 / 12 000 rather than milliseconds.

**Least confident part of this document, stated plainly.** A Debug build cannot measure CPU
scheduling at all, three sibling worktrees were building throughout this work and swing GPU timings
by up to 4×, and the axes do not all have an isolated benchmark to time. So the ratios were chosen
to reflect the implementations and sanity-checked against the structural census above — not
regressed against a stopwatch.

What follows from that, and what does not:

* the model is **authorable** (`AnimalCostModel` is a value type with a sanitiser), so a project
  that measures its own coefficients can substitute them and everything downstream follows;
* every conclusion drawn from the model in this document carries a **margin** (§1), because a
  conclusion that needed the ratios to be right to within a few per cent would not be supportable;
* `AnimalSchedulerStats::EstimatedCostUnits` is reported every frame, but **nothing compares it to a
  measured frame time**, so it is a budget-occupancy figure and not a drift detector. This document
  used to claim it was the latter. Wiring that comparison — units against
  `Scene::GetFrameTimeTail()` — is the natural way to calibrate, and is the obvious first follow-up.

Re-calibrating wants a Release build on an idle box, which is neither of the conditions this work
ran under.

## 5. The budget on real pixels

`AnimalBudgetVisualEvidenceTest`, ten animals (one hero + a 3×3 herd), 1280×720, all three lighting
paths, budget 1200 units against a 1620-unit population:

| quantity | budget off | budget on | note |
|---|---:|---:|---|
| strands built | 120 000 | **19 500** | 83.75 % of the work removed |
| hero coat pixels | 8 628 | **8 628** | byte-identical — the hero is never a candidate |
| herd coat pixels (front) | 24 691 | 23 338 | −5.5 %, inside the compensation band |
| all coat pixels (oblique) | 39 817 | 39 145 | −1.7 % |

Identical on Forward, Forward+ and Deferred, which is expected: the budget decides a strand count
and every path draws the geometry that count produced.

**The coat pixel count is not how you see this feature working, and that is worth writing down.**
The first version of this evidence test asserted the herd's coverage would *fall* when the budget
engaged. It does not — #1252's width compensation widens each surviving strand by `1/k` precisely
so the covered area holds. An early run measured the herd going 6 500 → 6 597 pixels, i.e. slightly
*up*, with fifteen sixteenths of its strands gone. So the honest split is:

* the **work** fell — `GroomRenderStats::StrandsDrawn`, which no compensation can hide;
* the **picture** survived — coverage held within a few per cent, and the hero did not move at all.

This is the coat-authoring rule (§5b) applied honestly: when an A/B changes the geometry, the pixel
set is part of the result rather than a fixed frame of reference.

Captures:
`OloEditor/assets/tests/visual/AnimalBudget[Off]_GL_{Forward,ForwardPlus,Deferred}_Front.png` and
`AnimalBudget[Off]_GL_Deferred_Oblique.png`. In the "on" frames the herd's coats read as a handful
of broad ribbons where the control shows a fine speckle, at the same silhouette; the hero is
visually indistinguishable between the arms.

## 6. Two measured-and-rejected alternatives

**A live frame-time budget.** The obvious design is to measure the last frame and spend the
remainder. It is rejected on reproducibility: the same scene, frame and camera would allocate
differently on a busy machine than on an idle one, so the population's trajectories stop reproducing
and every capture downstream becomes noise — which is criterion 1 failing silently rather than
loudly. The calibrated-unit model is what buys `RepeatedRunsOfTheSamePopulationAreBitIdentical` and
the manifest's `RepeatRmse: 0.0`.

**Coarsening one animal to its cap before touching the next.** Both this and the round-robin form
fit the budget; only round-robin spreads the loss. Driving one background animal to a sixteenth
while its neighbour stays at full rate is the within-frame twin of the starvation the counters fix
across frames, and it is visible as a herd with one obviously wrong member.

**An absolute starvation bound was tried and is wrong.** "No animal is below its desired step for
more than N consecutive frames" is unachievable at any budget small enough to matter: when the whole
population must be coarsened there is nobody to swap with. The first version of the contract test
asserted it and failed at 200 frames against a correct scheduler. The bound that *is* delivered is
relative — an animal held below its desired step while a peer of the same role sits at its own — and
`MaxStarvedFrames` is documented as a measure of pressure rather than of unfairness.

## 7. Criterion 4: the tiers

`QualityTieringSettings::AnimalFrameBudgetUnits`, applied to `RendererSettings` alongside the DDGI
knobs:

| tier | budget (units) | scheduling |
|---|---:|---|
| Low | 2 000 | on |
| Medium | 4 000 | on |
| High | 6 000 | on |
| Ultra | 12 000 | on |

**On at every tier, including Low** — the opposite of the DDGI rule above it, and deliberately.
Realtime GI is a whole feature to shed, so Low turns it off; the population budget is what makes a
herd affordable at all, so the weakest tier needs it most and the small allowance is the point.
`SchedulingIsOnAtEveryTierIncludingLow` pins that so a future tier edit cannot quietly invert it.

## 7b. What the live editor said, and the bug it found

`olo_groom_budget_stats` was added to read the pass's counters and the scheduler's decisions from a
running editor, because the alternative — grepping `OloEngine.log` for `built strand geometry ...
(stride N)` — only shows geometry-cache MISSES, so a steady-state frame reports nothing.

On `AnimalPopulation.olo`, Vulkan, Deferred, 41 animals:

| | value |
|---|---|
| `animalsConsidered` / `heroesCoarsened` | 41 / **0** |
| `animalsCoarsened`, `budgetExceeded` | 0, false — the population FITS the 6 000-unit High tier (1 192 units) |
| visibility `desiredCostUnits` → `scheduledCostUnits` | 184.9 → **245.7** |
| `animalsAtVisibilityFloor` | **18** |
| frame time p50 / p95 / p99 / max | 17.6 / 21.9 / 24.4 / 27.9 ms |

**The scheduled cost sitting ABOVE the desired cost is the tell**, and it exposed a real bug. Higher
cost means *finer*, and nothing in the scheduler can refuse to coarsen except a floor — so the
strand floor was holding coats against their own distance ladder, which is the "invisible distant
coats" guarantee firing. `AnimalsAtVisibilityFloor` reported **0** anyway.

The counter required `step > desired` — "the BUDGET pushed it to its cap" — and was blind to the
route that matters most: the ladder asking to thin past `MinVisibleStrands` and the floor refusing,
which needs no budget pressure at all. Both routes now count, and
`TheFloorCounterSeesTheLadderBeingRefusedAndNotOnlyTheBudget` pins it. The same scene now reports
18.

This is the second time on this issue that a counter looked right and was measuring the wrong
population — the first was `MaxStarvedFrames`. Both were found by reading a real frame rather than
the code.

## 8. The frame-time tail

`Scene` records every frame's delta into a 600-sample window (ten seconds at 60 Hz — the shortest
window in which a 99th percentile means anything; at 120 samples p99 *is* the second-worst frame
wearing a percentile's name). `Scene::GetFrameTimeTail(budgetMs)` returns p50/p95/p99/max, the mean,
and a **count** of frames over the budget, and the editor shows all of it beside the scheduler's own
counters.

Three deliberate choices:

* **The window fills whether or not the budget is enabled**, recorded before the early-out. A tail
  that only accumulated while the feature was on could not be compared against anything, and the
  off arm is the control for every measurement here.
* **It carries the caller's delta, not a wall-clock read.** Under a mock clock — which every capture
  in this feature's evidence runs under — the two differ, and a wall-clock tail would make a
  deterministic capture non-deterministic. `TheFrameTimeWindowIsFilledByTheRealSceneTick` asserts a
  fixed-dt run collapses the whole distribution onto the tick, which is what that guarantees.
* **It resets at a play-mode transition**, because a window spanning edit-mode and runtime frames
  describes neither.

The count sits beside the percentiles because they answer different questions: at a 600-frame
window p99 is six frames, so the percentile alone cannot tell one bad frame from six — and six is a
visible stutter while one is not. `ASingleSpikeIsInvisibleToP99AndVisibleToMaxAndTheOverBudgetCount`
pins that limit rather than leaving it to be rediscovered.

Per-tier tail numbers under a real workload are live-only and are reported in the PR's verification
matrix; what ships here is the instrument and the guarantee that something feeds it.
