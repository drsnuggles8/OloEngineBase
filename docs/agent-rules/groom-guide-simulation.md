# Groom guide simulation and body collision

Rules for changing the moving half of a coat: `Groom/GroomGuideSimulation.*`,
`Groom/GroomGuideInfluence.*`, `Groom/GroomBodyCollider.*`, `GroomSimulationComponent`, and the
`Scene::SimulateGroomGuides` step that drives them. Issue #1250.

The shape a coat is simulated *against* is #1249's; read
[groom-surface-binding.md](groom-surface-binding.md) first.

## The rules

**Length is a property of the groom, not of the frame rate.** The default solver
(`DynamicFollowTheLeader`) enforces it by *projection*, not by iteration: each particle is placed at
its exact rest distance from its already-final parent in one root-to-tip pass, so there is no
residual at any step size. If you replace that pass with anything iterative, criterion 1 stops being
a property of the algorithm and becomes a property of the tuning — which is the same as not having
it. `PositionBasedDistance` is kept only as the measurement reference; the numbers are in
[docs/analysis/groom-guide-simulation-1250.md](../analysis/groom-guide-simulation-1250.md).

**Never integrate a raw variable `dt`.** A fixed step with a bounded catch-up, the way `JoltScene`
does it. Arrears past `MaxSubsteps / FixedHz` are *dropped and counted*
(`GroomSimulationStats::StepsClamped`), never integrated — a four-second alt-tab must not run 240
steps. A coat permanently in arrears looks fine in a still frame and lags the body by a constant
offset in motion, which reads as a binding error, so the counter is the only way anyone finds it.

**A teleport resets; it does not stretch.** The caller owns the decision (`HasHistory`), and on a
reset the solver *returns without integrating*: it snaps the particles to the target shape, zeroes
the accumulator and emits zero motion. A frame that both re-seeds and steps applies a whole frame of
gravity to a coat that has just arrived, which is a one-frame sag on every teleport.

**Simulate the guides the STRAND budget did not select, and widen the deformer to cover them.**
`EvaluateGroomRootTransforms` only fills the curves it is given. A guide outside that set has no
deformed root, so it would be solved against the *bind pose* while the body moves — the coat then
lags its own animal by a whole animation. `Scene::SelectGroomSimulationGuides` merges the guide
curves into `m_SelectedCurves` before the evaluation runs; keep that order.

**What crosses the guide-to-strand boundary is a DISPLACEMENT, sampled by PARAMETER.** Not a
position: a strand is not at its guide, and a guide with twelve points and a strand with six must
agree about where "halfway up" is. Sampling by index drags short strands toward long guides' tips
and the coat splays under motion while every particle is perfectly inextensible.

**Every budget in this subsystem is a STRIDE, never a prefix.** The cook makes each group a
contiguous range of curves, so the first N guides of a role are one side of the animal. A prefix
budget leaves the other side still, which reads as a broken binding.

**The body proxy is FITTED, not authored**, and it is fitted with *percentiles*, not extrema. A
capsule that contains every last vertex of a hand is a sphere around the whole hand, and it pushes
the coat off the arm. The fit is a pure function of the surface and is cached against the same
identity keys the binding's compatibility verdict uses — re-fitting per frame is a full pass over
the body's vertices.

**Collision is resolved BEFORE the length projection, and that is a declared trade.** Length is then
exact and a particle may end a step a fraction of a *segment* inside the proxy;
`m_ColliderPadding` is the shell that buys it back. Inverting the order gives exact non-penetration
and a visibly stretched strand, and a stretched strand is the failure criterion 1 names. If you
change the order, change `GroomGuideSimulationTest.CollisionKeepsGuidesOutOfTheBody`'s declared
bound with it — it is written as the arithmetic, not as a magic number, for exactly this reason.

**Friction is applied to `Prev`, not to a velocity field.** This integrator's velocity *is*
`Curr - Prev`. Moving `Prev` toward the contact point along the normal kills the inward component
and scales the tangential one in one expression, so the two halves cannot disagree about which is
which.

**The reset control is a COUNTER, not a flag the scene clears.** `m_ResetKey` is compared against a
stored copy. A bool would make the reset a mutation of a component the tick is meant to read, it
would be lost on a save/load between the set and the consume, and two systems asking for a reset on
one frame would race to clear it.

**The simulation does not advance in edit mode.** `m_GroomSimulationDeltaSeconds` is zeroed at every
frame entry point and accumulated only inside `SimulateRuntimeStep`, which the pause gate wraps. A
scene must not change just from being open, and a paused frame must hold the pose it paused on. If
you need motion for a capture, drive it through `RunFrames` (runtime), not `RunEditorFrames`.

**The `.ologroom` format was deliberately NOT bumped.** Its `MinSupportedVersion` equals its
`CurrentVersion` by design (`GroomBinaryFormat.h`), so a new cooked section would refuse every groom
on disk today. Everything authored here lives on `GroomSimulationComponent` — scene YAML and the
save game, both of which have real backward-compatibility machinery — and the guide-to-strand
influence table is derived at runtime and cached. Keep it that way unless you are prepared to
re-cook every groom in the project.

**The coat-shadow volume is not part of this, and that is #1248's decision rather than an oversight.**
`GroomRenderPass::AcquireCoatVolume` releases the volume and reports not-ready for any DEFORMED groom, so a
bound coat has no self-shadow representation today — and a simulated coat is always a bound one. There is
therefore no frame-state incoherence between the solver and the shadow bake to introduce, because there is no
bake. When #1248's deformed path lands it will bake from the cache entry's geometry, which is the geometry the
interpolation just wrote, so it is coherent by construction — but check that rather than assume it.

**A new component means the whole cross-binding walk.** `GroomSimulationComponent` is generated into
the `AllComponents` tuple, the `OnComponent*` no-ops, scene YAML and the MCP field registry; the
save-game `Serialize` overload plus `RegisterAll`, and the editor inspector, are hand-written. Lua is
deliberately skipped, matching `GroomComponent` and `GroomBindingComponent` — see the component's own
comment for why, and say so rather than leaving it as an omission.

## Failure modes, and what they look like

| Symptom | Cause |
|---|---|
| The coat lags the body by a whole animation | A simulated guide outside the deformer's selection: solved against the bind pose |
| The coat splays under motion, solver numbers perfect | Guide displacement sampled by index instead of by parameter |
| One side of the animal is still | A budget taking a prefix instead of a stride, or that role's budget is zero |
| The coat ghosts / smears | `PrevDisplacements` kept across a re-seed; drop them so the sample aliases current and motion is exactly zero |
| The coat is rubbery through a frame spike | `PositionBasedDistance` selected, or an iteration count taken as a length guarantee |
| The coat hangs off the body like wet rope | `m_Stiffness` at or near zero: that term is the only one that knows the coat was authored |
| The coat is pushed off the arm | The fitted proxy inflated by a stray vertex — check `AxisHighPercentile`, not `MaxColliders` |
| Fingers poke through | `GroomColliderBuildStats::Truncated`: the cap dropped the smallest capsules |
| Nothing moves at all, no errors | `GroomGuideInfluenceTable::GetUnguidedStrands()` — the groom was exported with no guide flags, or a group has none |
| Two viewports disagree about where the fur is | Something stepped the solver per camera. It is stepped once per frame, in `DeformGroomAgainstSurface` |
| Two "identical" test replays put strands in different places | A rewind leaves the skeleton without bone history for one or two frames (one if the clip was ticking before, two if not), and the solver reseeds on each. A replay helper must hold the reseed for two frames, as `GroomAnimalsAcceptanceEvidenceTest::PlayFromStart` does, or it integrates from a history-dependent frame |

## Where the numbers are

`GroomGuideSimulationTest.SolverModelComparison` prints the length error each model leaves at
30/60/144 Hz and is the source of the analysis document's table. Re-run it rather than trusting the
table if you change the integrator:

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=GroomGuideSimulation.*
```
