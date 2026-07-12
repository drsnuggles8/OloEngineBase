# Character locomotion (issue #631)

The locomotion overhaul fuses the shipped-but-inert character-animation
primitives (IK solvers, retargeting, blend spaces, `JoltCharacterController`)
into a working locomotion stack. Five interlocking parts, landing as one epic:

1. **Root-motion consumption pipeline** (this doc, shipped first)
2. Runtime retargeting completion (see
   [animation-retargeting.md](animation-retargeting.md) deferrals #2/#3/#4)
3. Foot & hand ground-adaptation IK
4. Velocity-driven locomotion controller
5. Motion matching (ambition ceiling; graceful fallback = part 4)

## Part 1 — root-motion consumption

### The problem

`AnimationRootMotionSettings` (ExtractRootMotion / RootBoneIndex / masks /
DiscardRootMotion) existed on `AnimationAsset` and round-tripped through asset
YAML, but **nothing consumed it**: a clip authored with root motion visually
walked its mesh away from the entity origin and snapped back on loop-wrap; the
entity itself never moved.

### Data flow

```
AnimationAsset (authored settings)
      │ SetAnimationClip stamps →  AnimationClip::RootMotion   (runtime copy)
      ▼
per-tick extraction (wrap-aware, per contributing clip, masked, model-space)
   ├─ AnimationSystem::Update        — legacy two-clip path, blends current/next
   ├─ AnimationStateMachine::Update  — per state; cross-fades blend src/target
   │     └─ AnimationState::ExtractRootMotion → BlendTree::ExtractRootMotion
   │        (1D bracketing pair / 2D IDW weights — SAME weights as the pose)
   └─ AnimationGraph::Update         — base layer only (layer 0, Override),
         scaled by layer weight; published via GetLastRootMotion()
      ▼
pending delta on the component (entity/model space, overwritten per tick)
   ├─ AnimationStateComponent::m_RootMotion{Translation,Rotation}, m_HasRootMotion
   └─ AnimationGraphComponent::RootMotion{Translation,Rotation}, HasRootMotion
      ▼
Scene::UpdateRootMotion ("RootMotionApply" scheduler node)
   ├─ character-controller entity → JoltCharacterController::Move()/Rotate()
   │  (buffered displacement; PhysicsKick's character phase integrates it the
   │   SAME tick; the fence writes the resulting pose back to the transform)
   └─ plain entity → TransformComponent (rotation & scale applied to the delta)
```

Core math lives in `Animation/RootMotion.{h,cpp}` (`RootMotionUtils`):

- **`ExtractClipRootDelta`** — samples the root track between two clip times.
  Loop wrap accumulates `[S(D)−S(t0)] + (wraps−1)·[S(D)−S(0)] + [S(t1)−S(0)]`;
  the wrap jump `S(0)−S(D)` is a teleport, not motion. Rotation composes the
  same segments as local-space quat deltas. Non-looping clips clamp at the last
  key (zero further motion).
- **`ApplyMasks`** — translation masks are exact per-channel; rotation masks
  select euler components of the delta (exact for all-or-nothing masks,
  small-angle approximate for partial masks — fine for per-tick deltas).
- **`ToModelSpace`** — conjugates the root-local delta with
  `BindPoseGlobal[parent(root)] · PreTransform[root]`. A root-motion bone's
  ancestors are static by definition, so bind pose is authoritative; this is
  what makes a fox.gltf-style −90° X pre-rotation come out straight.
- **`MakeInPlaceRootPose`** — in-place-ification: the sampled root pose with
  the extracted (masked) motion removed, pinned against the clip's t=0
  reference sample. Applied at the two leaf samplers
  (`AnimationSystem`'s `SampleClipTRSPinned`, `BlendTree::SampleClipBoneTransforms`),
  so every path — single clips, cross-fades, blend trees, one-shots — pins
  consistently and the mesh never double-moves.

### Semantics

| ExtractRootMotion | DiscardRootMotion | Pose            | Entity          |
|-------------------|-------------------|-----------------|-----------------|
| false             | —                 | moves (legacy)  | static          |
| true              | false             | pinned in place | moves           |
| true              | true              | pinned in place | static (in-place conversion) |

- Masks: a component of **1 extracts** that channel to the entity (and removes
  it from the pose); **0 leaves it in the pose**. Typical: translation mask
  (1,0,1) keeps hip bob in the pose while XZ drives the character.
- Blends: each contributing clip extracts against its own settings/duration;
  deltas combine with the **same weights the pose blend uses** (legacy blend
  factor, transition factor, 1D bracket weight, 2D IDW weights — the 2D weight
  loop is factored into one shared helper so pose and motion cannot drift).
- Graph layers: base layer (layer 0, Override) only, scaled by layer weight.
  Additive/upper layers pose on top but never move the character. One-shot
  overlays play in place (pinned, not extracted).
- Editor: `Scene::OnUpdateEditor`'s preview runs extraction but nothing
  consumes the deltas — the pose previews in place and the entity never moves
  (edit-mode scene data stays untouched). Simulate mode runs no animation
  systems (pre-existing behavior).

### Scheduler seam

New channel `RootMotion`: written by `Animation` + `AnimationGraph`, read by
the new `RootMotionApply` node, which also `ReadsWrites(LocalTransforms)`. The
transform write gives `RootMotionApply → PhysicsKick` a real RAW edge — the
seam guaranteeing a character controller integrates THIS tick's extracted
motion (`SystemSchedulerTest.GameplayScheduleHonoursDocumentedSeams` pins it
via `DependsOn`, never positions). Controller entities must NOT be moved by
writing their transform pre-kick: the fence's controller→transform write-back
would clobber it — hence the `Move()/Rotate()` path.

### Adjacent fix

`CharacterController3DComponent`'s authored fields (slope limit, step offset,
gravity, air-control flags, layer) were a serialization-only data bag — the
Jolt controller always ran on hard-coded defaults. `JoltCharacterController::Create`
now applies them (layer 0 = "unset" keeps CHARACTER; see the layer-0 footgun
comment in JoltCharacterController.h).

### Tests

`OloEngine/tests/Animation/RootMotionTest.cpp` (`unit`) pins: linear/wrapped/
multi-loop/non-looping extraction, rotation accumulation across wrap, masks,
blend semantics, model-space conversion, in-place pinning, configured
extraction (flag/discard/out-of-range), the AnimationSystem cross-fade path,
state-machine transitions, 1D/2D blend-tree weight parity, and graph layer
rules. The scheduler seam is pinned in `SystemSchedulerTest`. The Functional
walk test and the visual-evidence screenshot test land with parts 3–4 (they
exercise the full stack).

## Part 2 — runtime retargeting completion

Shipped as deferrals #2/#3/#4 of
[animation-retargeting.md](animation-retargeting.md) (see that doc for the
detail): bone-length-ratio per-bone translation transfer
(`RetargetOptions::TranslationMode::PerBoneRatio`), the live
`RetargetingComponent` + `RetargetingSystem` ("Retargeting" scheduler node,
lazy bake-and-splice into `m_AvailableClips`, full cross-binding), and the
editor authoring UI with the AutoDetect role table + per-bone overrides.
Root-motion settings survive the bake with the root bone index remapped into
the target skeleton — the parts 1+2 interlock
(`AnimationRetarget.RetargetClipCarriesRootMotionSettingsWithRemappedRoot`).

## Part 3 — foot & hand ground-adaptation IK

`FootIKComponent` (+ runtime `FootIKStateComponent` twin) and
`ApplyFootIKPostPass` (`Animation/IK/FootIKPostPass.{h,cpp}`), ordered after
`ApplyIKPostPass` and before spring bones in both animation systems:

- **Ground probing** is resolved Scene-side (`Scene::ResolveFootIK`): one ray
  per foot straight down from LAST tick's foot pose, cast pre-PhysicsKick
  (legal — the previous step fenced last tick; NEVER inside the physics
  shadow). No physics (edit mode) → the cache clears and feet stay animated.
  The pass itself never touches Jolt, which keeps every geometric contract
  unit-testable with injected ground data (`FootIKTest`).
- **Ground conformance** keeps the clip's lift above the LOCAL ground:
  `targetY = groundY + FootHeight + max(0, modelFootY − FootHeight)`.
- **Pelvis adaptation** lowers the hips (exponentially smoothed, clamped by
  `MaxPelvisDrop`) so the lowest foot target is reachable.
- **Foot planting** world-locks a grounded, slow, low foot during stance and
  eases it back over `UnlockBlendTime` when the animation lifts it.
- **Slope alignment** tilts the foot to the ground normal (clamped by
  `MaxSlopeAngle`) with an optional toe counter-roll.
- **Hand IK** LimbIK-solves each enabled hand onto a world target or a target
  entity (prop/ledge), resolved Scene-side like the IK targets.
- Leg/arm chains need **3 chain bones** (ankle+knee+hip) — 2 bones is a single
  rigid segment that can only pivot, never bend.

## Part 4 — velocity-driven locomotion controller

`LocomotionComponent` (+ runtime `LocomotionStateComponent`) and
`LocomotionSystem` ("Locomotion" scheduler node before the animation systems;
the graph evaluation consumes its writes via the `AnimationParams` channel):

- **Velocity source**: measured by default (character-controller linear
  velocity, else transform deltas), or `DesiredVelocity` when gameplay code
  steers a root-motion character (`UseDesiredVelocity`).
- Writes smoothed planar **Speed**, local-space **MoveX/MoveY** (normalized by
  `DirectionReferenceSpeed`), **Gait** (0/1/2 with enter/exit hysteresis so a
  boundary-hovering speed can't flicker), and **Turn** (smoothed yaw rate).
  Parameters are Defined on the graph if missing — a graph references only the
  names it cares about.
- **Stride warp** scales the ACTIVE base-layer state's playback rate by
  `speed / gaitClipSpeed` (clamped by `MaxStrideScale`, authored base speed
  remembered + restored) so a velocity-driven character's stride matches its
  ground speed. Root-motion characters get their movement FROM the clips, so
  their feet cannot slide by construction.

## Part 5 — motion matching (open ambition; documented fallback)

Not implemented in this slice — the issue's sanctioned minimum is parts 1–4
with a documented graceful fallback, which is exactly the part-4 parametric
controller: gait-hysteresis blend-space selection + stride warp on top of
root-motion-driven movement. A future motion-matching slice would slot in as
an alternative pose provider at the AnimationGraph layer (feature database
built from the clip set, per-tick nearest-neighbor search against the desired
trajectory from `LocomotionComponent`, inertialized blend to the matched
pose), degrading to this parametric controller when no database is authored —
the LocomotionComponent parameter surface was designed so both providers
consume the same inputs.

## Verification

- CPU contracts: `RootMotionTest` (34), `FootIKTest` (10),
  `AnimationRetargetTest` (21, incl. the per-bone-ratio + root-motion-carry
  additions).
- Functional (`Scene::OnUpdateRuntime`): `LiveRetargetingTest` (3),
  `LocomotionControllerTest` (5), and the capstone
  `HumanoidLocomotionWalkTest` — a Biped-named source rig live-retargeted onto
  a Mixamo-named humanoid walks by root motion across a real Jolt floor and
  up onto a raised step with the feet raycast-adapted and the pose pinned.
- Visual evidence: `LocomotionVisualEvidenceTest` (L7) writes
  `OloEditor/assets/tests/visual/Locomotion_{Side_Before,Side_After,Front,TopDown}.png`
  and pins driver-independent pixel contracts (torso crossed the fixed frame,
  feet present + under the body + travelled with it); scheduler seams pinned in
  `SystemSchedulerTest` via `DependsOn`.
