# Reset world-anchored renderer state in the test fixture — a new `Scene` does not

Renderer state that lives in `Renderer3D`'s process statics is **not** cleared by building
a new `Scene`. Production clears it at `Scene::OnRuntimeStart` / `Scene::OnSimulationStart`;
`RendererAttachedTest` drives `OnUpdate*` directly and passes through neither. So each test
in a fixture inherits whatever the previous test left at the same world coordinates, and a
golden captured that way encodes the residue — it matches only while the sibling test that
produced it ran first, in the same process.

Issue #1086 is the worked example.

## The symptom, and why it read as two other things

`WaterFoamSprayRainVisualEvidenceTest.SprayEmitsOnABuiltSeaAndNotOnACalmOne` failed on
`origin/master`, deterministically, at RMSE 12.13 and 8.75 against a threshold of 6. The
issue posed it as a fork: either a deliberate visual change had landed without
`--olo-golden-rebase`, or the renderer had regressed. It was **neither**, and the history
supported the wrong answer convincingly — the goldens landed in one commit and the very
next commit on the same day changed `WaterSpraySystem`'s drain guard without rebasing
them. That commit is not the cause. (Its other half, the spray threshold source, is
provably inert here: the scene has a single water tile, so the foam and spray contests have
the same winner and the value is unchanged.)

The tell is an ordering table, not a `git log`:

| run | result |
|---|---|
| the whole 4-test suite | passes |
| `SprayEmits…` alone | **fails**, 12.129336668385715 / 8.7487513576673912 |
| `FoamDrift…` + `SprayEmits…` | passes |
| `AdvectedFoam…` + `SprayEmits…` | **fails**, 12.1256 / 8.7484 |

`FoamDriftSequenceFromAFixedCamera` runs ~240 ticks with foam accumulating into the
world-anchored disturbance field. The spray poses were captured with that field already
built, so they matched in file order and diverged whenever the test ran alone — which is
exactly what a `--gtest_filter` or a `ctest` shard does. Side by side the isolated capture
shows the same waves, the same camera and the same spray cluster, with visibly thinner and
patchier foam.

## The rule

**A fixture that builds a `Scene` per test must also reset the process-static renderer
state that scene load resets.** For water that is the four systems `Scene.cpp` resets
together:

```cpp
WaterDisturbanceSystem::Reset();
WaterWakeSystem::Reset();
WaterRainRippleSystem::Reset();
WaterSpraySystem::Reset();
```

Mirror the production site rather than picking the one system that looks relevant — they
are reset together because the field is shared. `WaterDisturbanceSystem::Reset`'s own doc
comment already stated the invariant ("Must be called on scene load / topology reset: the
field is world-anchored and persists across frames"); what nothing said is that a test
fixture is a scene load for this purpose.

## Two things worth copying from the diagnosis

**Prove order-independence before rebasing, not after.** With the reset in place, all
three orderings still failed against the *old* goldens — but the in-suite and
after-a-sibling numbers became byte-identical, which they were not before. That is the
evidence that the rebase is recording a stable frame rather than a differently-polluted
one. Rebasing first and observing green proves nothing: the golden was written from
whatever ran.

**Rebase writes every pose, so check which ones needed it.** `--olo-golden-rebase`
rewrote three PNGs; only two had actually moved. The third (`RainOn`) still passed against
its committed golden in both orderings, so its byte change was the run-to-run churn
`docs/process/task-loop.md` warns about and it was reverted. Revert every golden you cannot
show needed to move — a renderer PR carrying meaningless binary diffs implies a visual
change that did not happen.

## Reach

The fix here is scoped to one test file. The hazard is not: any fixture built on
`RendererAttachedTest` that exercises a subsystem with world-anchored process statics has
it, and the failure mode is silent — the suite is green in file order and red in a shard.
When a visual-evidence golden fails "deterministically in isolation", run the ordering
table above before reading a single line of renderer history.

Related: [runtime-scene-switching.md](runtime-scene-switching.md) for the production-side
rule this is the test-side corollary of, and
[procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md) for when
a golden legitimately moves.
