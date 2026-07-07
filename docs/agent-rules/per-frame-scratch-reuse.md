# Reuse per-frame scratch buffers as persistent instance state, not function locals

Short rule for any per-tick hot path that builds a temporary `std::vector`
(or similar) inside a function that runs once per entity per frame — or
worse, once per entity per **sub-step** per frame (issue #445).

## The trap

`AnimationGraph::Update()` evaluated each layer's pose into a fresh
`std::vector<BoneTransform> layerTransforms;` **declared inside the per-layer
loop** — reallocating on every layer of every skeleton of every tick.
`AnimationStateMachine::Update()`'s cross-fade path had the same shape for
its `currentTransforms`/`targetTransforms` locals, allocating on every ticked
frame of every transition. Both looked "obviously fine" in isolation (small,
short-lived, scoped to the call) — the cost only shows up as aggregate heap
churn across many skinned characters every frame, which unit tests never
notice because they don't measure allocation counts.

Meanwhile, `Renderer3D::DrawAnimatedMesh` a layer below already wrote bone
matrices into `FrameDataBuffer` via offset+count instead of copying a
`std::vector` — the render side had already solved this class of problem.
The gap was one layer up, in the *animation-compute* code that produces the
data the renderer then submits correctly.

## The rule

**If a hot-path function is called every tick against a stable, per-instance
owner (an entity's own `AnimationGraph`, `AnimationStateMachine`, etc.),
promote its scratch containers to persistent members of that instance and
`resize()` them in place instead of declaring fresh locals.** This is safe
exactly when three things hold — check all three, not just the first:

1. **Every callee that fills the buffer always fully overwrites
   `[0, count)`, never partially.** A `resize()` that only *grows* — never
   clears — is safe to reuse iff nothing downstream depends on
   previously-unwritten elements being empty/default. Audit every write path
   (here: `BlendTree::FillBindPose`, `AnimationStateMachine::Update`,
   `BlendTree::Evaluate*`, `BlendTree::BlendBoneTransforms` — all resize +
   fully overwrite). If a path only *conditionally* fills the buffer (e.g. a
   null target state skips its write), explicitly `.clear()` it first so the
   reused buffer can't leak the previous call's contents into this one — see
   `AnimationStateMachine::Update`'s cross-fade `targetTransforms.clear()`.
2. **The instance is not shared across concurrent callers.** Each entity
   must own its own instance (here: `Scene.cpp` `Clone()`s a fresh
   `AnimationGraph` per entity), not a shared template/asset-level object.
3. **The call site is not running on a `.Parallelizable()` worker path**
   without its own synchronization. Check `Scene::GetGameplayScheduler()`'s
   registration for the system that drives the call (here: `AnimationGraph`
   is not marked `.Parallelizable()`, so it's safe without a lock).

## Guard

`AnimationStateMachineTest.SequentialCrossFadesDoNotLeakScratchBetweenTransitions`
and `AnimationStateMachineTest.MultiLayerGraphUpdateReusesScratchAcrossLayersAndTicksWithoutBleed`
(`OloEngine/tests/AnimationStateMachineTest.cpp`) target the actual hazard
reuse introduces — stale data bleeding across transitions/layers/ticks —
rather than re-testing pose correctness the pre-existing tests already cover.
A regression that breaks rule 1 above (a callee stops fully overwriting its
buffer) would show up as one of these tests reading a stale prior value.
