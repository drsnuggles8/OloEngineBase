# Multi-animal scheduling: the population budget, and what it may never buy back (#1258)

Read before touching `OloEngine/src/OloEngine/Scene/AnimalScheduler.{h,cpp}`, `AnimalBudgetComponent` / `AnimalPathComponent`, `Scene::GatherAnimalWorkItems`, the `RendererSettings::Animal*` knobs, or `Core/FrameTimeTail.{h,cpp}`. The scheduler sits above every per-entity LOD ladder.

## Rules

1. **Spend the budget in model units, never a live clock reading.** A clock-reading scheduler allocates differently on busy machines and breaks reproducibility. `AnimalCostModel` coefficients are structural ratios, not microseconds, so any conclusion from them must carry a margin (how wrong the model may be before it flips). [Details](multi-animal-scheduling-budgets-details.md#the-rules)
2. **Keep four axes with four allowances (Deformation, Simulation, Visibility, Shadow), never one quality scalar.** `AnimalSchedulingCensus.TheDominantAxisMovesWithApparentSizeSoOneQualityScalarCannotBeRight` shows the dominant axis changes with apparent size. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
3. **Split the allowance by normalised weight per axis, not clamped.** An expensive simulation cannot eat the visibility allowance; `2/1/1/1` equals `0.4/0.2/0.2/0.2`. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
4. **Bound a reduced tick rate in screen space, not by distance.** `MaxDeformationStepForPoseBound` derives the cap from the animal's authored motion and apparent size. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
5. **Keep `AnimalBudgetComponent::m_FullRateMotionMetres` authored.** Measuring it needs the pose the budget exists to skip, and it belongs to the clip set (the sprint cycle), not the current gait. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
6. **Allow one halving per hold window on every axis.** Rate-limit the request to `current + 1` before the hold sees it; refining stays immediate (`GroomLod::ApplyHold`). [Details](multi-animal-scheduling-budgets-details.md#the-rules)
7. **Apply `MinVisibleStrands` as a `min()` against the desired step as well as the allocated one, and do not force a groom authored below the floor up to it.** The distance ladder alone can produce invisible distant coats. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
8. **Prevent starvation by service order with a relative bound.** The per-axis starvation counter is the second sort key and `StarvationFrames` removes an animal from the candidate set while a same-role peer is eligible; drop the rule for a pass where every candidate is over the bound. Do not assert an absolute "nobody below desired for N frames" bound (unachievable). [Details](multi-animal-scheduling-budgets-details.md#the-rules)
9. **Exhaust role groups in turn, never interleaved.** Every Background animal is at its cap before a Featured one gives way. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
10. **Report an unservable budget (`AnimalSchedulerStats::BudgetExceeded`), never absorb it in the hero.** With `AnimalProtectHero` the hero is not a candidate. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
11. **End the service-order tie-break with the UUID.** Otherwise ties fall back to EnTT gather order, so loading the same scene twice schedules differently. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
12. **Repair a NaN apparent size before the comparator sees it.** `ServiceOrder` holds a sanitised copy; NaN breaks strict weak ordering and makes `std::sort` undefined. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
13. **Keep scheduler state in `Scene`, keyed by UUID, never in a pass.** A pass runs per camera, but hysteresis and starvation counters advance per frame (same as `GroomLodState`). [Details](multi-animal-scheduling-budgets-details.md#the-rules)
14. **Reset the scheduler state when the budget is turned off.** Off is the A/B control arm. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
15. **Make `AnimalPathComponent` closed form in elapsed time, facing from the analytic derivative.** No accumulation (drifts with `dt`, like `BenchmarkManifest::CameraPoseAtFrame`) and no finite difference (undefined at `dt == 0`). [Details](multi-animal-scheduling-budgets-details.md#the-rules)
16. **Keep the path a Lissajous figure, not a circle.** Circles hold constant camera distance, so the distance ladders never move. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
17. **Report the tail, not the mean.** `FrameTimeTail` gives p50/p95/p99/max plus an over-budget frame count (at a 120-sample window p99 is one frame). [Details](multi-animal-scheduling-budgets-details.md#the-rules)
18. **Use nearest-rank percentiles in `FrameTimeTail`, matching `perf_trend.py`.** [Details](multi-animal-scheduling-budgets-details.md#the-rules)
19. **Use minimum-of-N for microbenchmarks and the tail for frame pacing; never swap them.** [Details](multi-animal-scheduling-budgets-details.md#the-rules)
20. **Price draw calls in `AnimalCostModel` and subtract them off the top; keep the census as a test.** Otherwise all allowances are zero and `BudgetExceeded` fires on frame one (geometry-lod-measure-the-unreachable-cost.md rule 10). [Details](multi-animal-scheduling-budgets-details.md#the-rules)
21. **Schedule a bound coat's body.** The gather prices the bones of the binding's target and `ShouldPoseAnimalThisFrame` gates the body by its coat's schedule (`m_AnimalBodySchedules`) unless the body has its own budget (#1259); `BoundCoatPopulationFixture` holds it with a bindings-off control. [Details](multi-animal-scheduling-budgets-details.md#the-rules)
22. **When adding an `AnimalBudgetPolicy` / `AnimalScheduleState` / `AnimalSchedule` field, order it 4-byte-then-1-byte with the tail named explicitly, update the `sizeof` `static_assert`, and list the type in `BitwiseEqualLayoutTest`.** They are compared with whole-object `memcmp`. [Details](multi-animal-scheduling-budgets-details.md#adding-a-field)
23. **Add a new `AnimalWorkAxis` knowing `AnimalWorkAxisCount` sizes five arrays and the census prints four columns.** `AxisUnitWork` and `AxisCoefficient` switch on it with no default case so a new axis is a compile error. [Details](multi-animal-scheduling-budgets-details.md#adding-a-field)
24. **Follow the CLAUDE.md cross-binding walk for a new component field.** Generated: `AllComponents` tuple, `OnComponent*` no-ops, scene YAML, MCP field registry. Hand-written: save-game `Serialize` plus `RegisterAll`, editor inspector. Lua is deliberately skipped (as `GroomComponent`, `GroomLodComponent`); say so. [Details](multi-animal-scheduling-budgets-details.md#adding-a-field)
25. **Read `olo_groom_budget_stats` on a live editor, `animalBudget.enabled` first.** `groom` is what the pass drew, `animalBudget` what the scheduler decided, `frameTime` the rolling 600-frame window; `false` means a thinned coat is its own distance ladder. Never grep the log for `built strand geometry ... (stride N)` (cache misses only). [Details](multi-animal-scheduling-budgets-details.md#reading-it-on-a-live-editor)
26. **Diagnose a wrong schedule by counter.** `AnimalsConsidered` 0 means no `AnimalBudgetComponent`; `CoarsenedByRole[Hero]` non-zero means `AnimalProtectHero` is off; `BudgetExceeded` with high `AnimalsCapHeld` means the population outgrew the caps; bald distant animals with `AnimalsAtVisibilityFloor` 0 means the floor is not reaching `MaxStep`; judder with `AnimalsAtPoseStepCap` 0 means `m_FullRateMotionMetres` is too low; flicker is `StepChanges` near `AnimalsConsidered * 4`; read `FrameTimeTail::Query().P99Ms` not the mean. `MaxStarvedFrames` measures pressure, not unfairness; copy `AnimalSchedulerStarvation.NoAnimalIsPassedOverWhileAPeerSitsAtItsDesiredStep`. [Details](multi-animal-scheduling-budgets-details.md#what-a-wrong-schedule-looks-like-and-which-counter-names-it)

## Commands and checks

Re-run the census instead of trusting the analysis tables after changing a coefficient:

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=AnimalSchedulingCensus.*
```

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[multi-animal-scheduling-budgets-details.md](multi-animal-scheduling-budgets-details.md).
