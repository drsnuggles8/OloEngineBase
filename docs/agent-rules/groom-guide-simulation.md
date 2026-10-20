# Groom guide simulation and body collision

Rules for changing the moving half of a coat: `Groom/GroomGuideSimulation.*`, `Groom/GroomGuideInfluence.*`, `Groom/GroomBodyCollider.*`, `GroomSimulationComponent`, and the `Scene::SimulateGroomGuides` step (issue #1250). Read [groom-surface-binding.md](groom-surface-binding.md) first.

## Rules

1. **Enforce strand length by projection, not iteration.** The default solver (`DynamicFollowTheLeader`) places each particle at its exact rest distance from its final parent in one root-to-tip pass; anything iterative makes length a tuning property. `PositionBasedDistance` is the measurement reference only. Numbers: [docs/analysis/groom-guide-simulation-1250.md](../analysis/groom-guide-simulation-1250.md). [Details](groom-guide-simulation-details.md#the-rules)
2. **Never integrate a raw variable `dt`.** Use a fixed step with bounded catch-up as `JoltScene` does; drop and count arrears past `MaxSubsteps / FixedHz` in `GroomSimulationStats::StepsClamped`. [Details](groom-guide-simulation-details.md#the-rules)
3. **On a teleport, reset and return without integrating.** The caller decides (`HasHistory`); the solver snaps particles to the target shape, zeroes the accumulator and emits zero motion. Re-seeding and stepping in one frame sags the coat. [Details](groom-guide-simulation-details.md#the-rules)
4. **Simulate the guides the strand budget did not select, and widen the deformer to cover them.** `Scene::SelectGroomSimulationGuides` merges guide curves into `m_SelectedCurves` before `EvaluateGroomRootTransforms`; keep that order or the guide solves against the bind pose. [Details](groom-guide-simulation-details.md#the-rules)
5. **Pass a displacement sampled by parameter from guide to strand,** not a position and not by index (index sampling splays the coat). [Details](groom-guide-simulation-details.md#the-rules)
6. **Budget by stride, never prefix,** for every budget here; groups are contiguous so a prefix leaves one side still. [Details](groom-guide-simulation-details.md#the-rules)
7. **Fit the body proxy with percentiles, not extrema, and cache it** against the binding's compatibility identity keys. Never push a particle further out than its target (#1533), or short fur lifts off a slim crown or ear base. [Details](groom-guide-simulation-details.md#the-rules)
8. **Resolve collision before the length projection.** Length stays exact and a particle may end a fraction of a segment inside the proxy; `m_ColliderPadding` buys it back. If you reorder, change the bound in `GroomGuideSimulationTest.CollisionKeepsGuidesOutOfTheBody` (written as arithmetic) with it. [Details](groom-guide-simulation-details.md#the-rules)
9. **Apply contact friction last, to `Prev`, and keep DFTL's hand-back from crossing a contact** (#1533). Remove the approaching normal part, keep a separating part, scale the tangential part; skip hand-back for a pair with a held particle. Otherwise a coat never rests (`StrandsAcrossTwoCollidersComeToRestAtAnyFriction`). [Details](groom-guide-simulation-details.md#the-rules)
10. **Use a counter (`m_ResetKey`, compared to a stored copy) for reset, not a flag the scene clears.** A bool is lost across save/load and races between systems. [Details](groom-guide-simulation-details.md#the-rules)
11. **Do not advance the simulation in edit mode, but carry particles by their target's motion** (#1533). `m_GroomSimulationDeltaSeconds` is zeroed at every frame entry and accumulated only in `SimulateRuntimeStep` (behind the pause gate); the skeleton still animates in edit mode, so a no-step solver call moves particles with their targets. [Details](groom-guide-simulation-details.md#the-rules)
12. **Give a short coat a stiff solver.** Default `m_Stiffness` 90 sags a free particle g/k about 11 cm; the dog showcase uses 1200 with gravity −4 and long-hair group stiffness scale ~0.5. [Details](groom-guide-simulation-details.md#the-rules)
13. **Do not bump the `.ologroom` format for simulation data.** `MinSupportedVersion` equals `CurrentVersion` (`GroomBinaryFormat.h`); authored data lives on `GroomSimulationComponent`, the influence table is derived at runtime. The one exception is the per-group `StiffnessScale` in section 9 (#1533), which multiplies `m_Stiffness` per guide. [Details](groom-guide-simulation-details.md#the-rules)
14. **Do not read particles for the coat-shadow volume;** it bakes from drawn strands or at rest ([groom-deformed-coat-self-shadowing.md](groom-deformed-coat-self-shadowing.md), [groom-coat-rest-bake.md](groom-coat-rest-bake.md)). [Details](groom-guide-simulation-details.md#the-rules)
15. **Walk the cross-binding list for `GroomSimulationComponent`.** Generated: `AllComponents`, `OnComponent*` no-ops, scene YAML, MCP field registry. Hand-written: save-game `Serialize` plus `RegisterAll`, editor inspector. Lua is deliberately skipped (as `GroomComponent`, `GroomBindingComponent`); say so. [Details](groom-guide-simulation-details.md#the-rules)
16. **Diagnose by symptom.** Coat lags a whole animation: guide outside the deformer selection. Splay with perfect solver numbers: index sampling. One side still: prefix budget or zero role budget. Ghosting: `PrevDisplacements` kept across a re-seed (drop them). Rubbery on spikes: `PositionBasedDistance` selected. Hangs like wet rope: `m_Stiffness` near zero. Pushed off the arm: stray vertex inflating the proxy (check `AxisHighPercentile`, not `MaxColliders`). Fingers poke through: `GroomColliderBuildStats::Truncated`. Nothing moves, no errors: `GroomGuideInfluenceTable::GetUnguidedStrands()`. Two viewports disagree: the solver must step once per frame, in `DeformGroomAgainstSurface`. Replays differ: a rewind leaves no bone history for one or two frames; hold the reseed two frames as `GroomAnimalsAcceptanceEvidenceTest::PlayFromStart` does. [Details](groom-guide-simulation-details.md#failure-modes-and-what-they-look-like)

## Commands and checks

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=GroomGuideSimulation.*
```

`GroomGuideSimulationTest.SolverModelComparison` prints the length error per model at 30/60/144 Hz; re-run it after changing the integrator.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[groom-guide-simulation-details.md](groom-guide-simulation-details.md).
