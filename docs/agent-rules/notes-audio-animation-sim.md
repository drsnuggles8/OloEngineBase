# Subsystem notes — audio/SoundGraph, animation, and the fixed-timestep sim loop

Accumulated gotchas from SoundGraph parameter routing and spatialization, animation pose sampling
and retargeting, morph targets, the deterministic tick, and scene authoring. Reference notes, not
failure postmortems — see [README.md](README.md).

Salvaged from worktree-scoped memory (see `docs/process/task-loop.md` Phase 7 for why that is now
the wrong destination).

---

## 1. Map animation channels to bones by NAME, never by index

A clip's `AnimationClip::BoneAnimations` are stored in exporter / `aiAnimation->mChannels` order and
cover only the *animated subset* of nodes, whereas skeleton bone indices come from a depth-first
`ProcessSkeleton` traversal. **`clip.BoneAnimations[i]` is not skeleton bone `i` on any real
imported rig** — mapping by index scrambles the pose (head animation on a leg).

For each skeleton bone use `clip->FindBoneAnimation(boneName)` (O(1), internally cached). **Bones a
clip does not key must fall back to the bind-pose local transform** (`m_BindPoseLocalTransforms`, in
TRS form), never identity — identity collapses the bone to the origin.

Issue #543 fixed the lone by-index outlier, the animation-**graph** path, by threading a
`PoseEvalContext { std::span<const std::string> BoneNames; std::span<const BoneTransform> BindPose; }`
down through `AnimationGraph::Update` → `AnimationStateMachine::Update` → `AnimationState::Evaluate`
→ `BlendTree::Evaluate*` → `SampleClipBoneTransforms`. **Any new pose-sampling path must mirror
this.**

> The regression guard (`AnimationGraphBoneMappingTest.cpp`) builds a clip whose channel order
> deliberately differs from skeleton bone order. The single-`Bone0` fixtures in
> `BlendTreeTest`/`AnimationStateMachineTest` **cannot** catch this, because there index == name.

## 2. Retargeting: re-base the rotation, don't copy it

- **Reuse `BoneTransform`** (TRS: Translation / Rotation quat / Scale, in `Animation/BlendNode.h`) —
  the canonical per-bone pose element, already used by IK and BlendUtils. Don't invent a parallel
  pose type. `BlendUtils::DecomposeMatrix` converts a local transform back to TRS.
- **Rest poses live in the skeleton** — `m_BindPoseLocalTransforms` / `m_BindPoseMatrices` are only
  populated by `SetBindPose()`, which loaders call after building the skeleton. **A skeleton built
  in a test must call `SetBindPose()` first.**
- **Correct rotation retargeting is a delta re-base:**
  `R_target = R_target_rest · (R_source_rest⁻¹ · R_source_anim)` — transfer the animation as a delta
  from the source rest pose, applied on top of the target rest pose. A naive
  `R_target = R_source_anim` only works when both rest orientations are identical. Always
  `glm::normalize` the result.
- **Empty keyframe channels sample to defaults, not "unchanged".** `SampleBonePosition` returns
  `vec3(0)` for an empty key list (scale → `vec3(1)`, rotation → identity). So a baked retargeted
  clip must emit **at least one constant key per channel** (the target rest value) for every mapped
  bone, or the bone collapses to the origin at play time.
- **Bake, don't add a component, for a first slice.** Baking a target-ready clip and playing it
  through the existing `AnimationSystem` avoids the cross-binding touch-points an ECS component
  would require.

> For a public API writing into a caller-sized `std::span`, an `OLO_CORE_ASSERT` gives **no
> release-build protection** (see [notes-core-and-threading.md](notes-core-and-threading.md) §8) —
> clamp the loop to `std::min(expected, span.size())` and keep the assert as contract documentation.

## 3. Morph targets feed one global loop from two animation paths

Both the `AnimationStateComponent` path (samples `clip->MorphKeyframes` inline in `Scene.cpp`) and
the `AnimationGraphComponent` path (`CollectActiveMorphClips()` →
`MorphTargetSystem::SampleMorphKeyframes()`) feed the **single** morph-evaluation loop in
`Scene::OnUpdateRuntime`.

- **Clip-time conversion on the graph path:** the state machine's `m_CurrentStateTime` is
  *speed-scaled* (it loops at `Duration/Speed`). Sample with
  `GetCurrentStateNormalizedTime() * Clip->Duration` — the same time `AnimationState::Evaluate`
  feeds the bone channels — **not** raw `m_CurrentStateTime`.
- **Ordering matters:** the global morph block runs **after** the animation-graph update, so weights
  sampled this frame deform this frame. Moving it earlier reintroduces a one-frame lag.
- **The eval loop needs a GL context** (`vb->SetData`), so it is not driven in headless Functional
  tests — assert sampling, then call `MorphTargetSystem::EvaluateMorphTargets` directly for the CPU
  deformation check.
- `CollectActiveMorphClips` only handles **SingleClip** states; blend-tree morph sampling and morph
  cross-fade during transitions are deferred. The GPU path (`EvaluateMorphEvaluator::EvaluateGPU`)
  is still stubbed — it binds SSBOs but never compiles or dispatches; the CPU path writes deformed
  verts back into `MeshSource` and re-uploads the whole vertex buffer each frame.

## 4. The fixed-timestep accumulator must NOT live in `OnUpdateRuntime`

`Scene::OnUpdateRuntime(Timestep ts)` has a hard **"advance the sim by exactly `ts`"** contract —
~100 Functional tests, the headless `OloServer` and `RendererAttachedTest` all hand-feed a custom
per-call dt and depend on one step per call. Burying an accumulator inside it silently changes step
counts and breaks all of them.

So the accumulator lives in a **separate entry**, `Scene::OnUpdateRuntimeFixed(frameTs, fixedDt)`,
called only by the windowed real-time hosts (the editor Play branch, `OloRuntime`). Server and tests
keep `OnUpdateRuntime`.

`OnUpdateRuntime` was split into `UpdateStreaming()` / `SimulateRuntimeStep(ts)` / `RenderRuntime(ts)`.
The Fixed entry runs `SimulateRuntimeStep(fixedDt)` N times (catch-up clamped at
`s_MaxFixedStepsPerFrame = 15`) but `RenderRuntime` **once** — fixed-rate gameplay, display-rate
rendering. Both entries funnel through `SimulateRuntimeStep`, which bumps the rollback-addressable
`m_SimulationTick`.

`fixedDt = 1/60` matches `JoltScene::m_FixedTimeStep`, so a per-sub-step `Simulate(1/60)` aligns with
Jolt's own accumulator. The gameplay RNG is re-seeded at `Scene::OnRuntimeStart` — see
[notes-core-and-threading.md](notes-core-and-threading.md) §13 for why it is deliberately not
cryptographic.

## 5. Render interpolation overwrites and restores the live transform

Issue #502 decouples display rate from the fixed tick. `OnUpdateRuntimeFixed` snapshots every
entity's **local** transform into `m_InterpPrev` before each fixed step and `m_InterpCurr` after the
last; `RenderRuntime` overwrites each live local with the blended pose (`glm::mix` for T/S,
`glm::slerp` for rotation), propagates, renders, then **restores and re-propagates**.

**Why overwrite-then-restore rather than threading `alpha` through draw calls:** render reads go
through *both* `GetWorldTransform()` and direct `TransformComponent::GetTransform()`
(terrain/water/foliage/decals). Overwriting the local makes every read see the interpolated pose
with no per-call-site changes.

Four things not to regress:

- **Restore stores a full `TransformComponent` copy, not just TRS.** `Rotation`/`RotationEuler` are
  private and `SetRotation()` re-derives the Euler representation, so restoring via the setter drifts
  `RotationEuler` even when the quat is bit-identical. `*tc = savedCopy` restores exactly, including
  the private Euler and the matrix cache.
- **The primary camera is excluded** from the geometry overwrite — the display-rate fly-cam mutates
  its live local *during* render, so restoring from the snapshot would erase that movement. A
  gameplay (non-fly) camera is blended directly into `cameraTransform` instead.
- **Interpolation never touches persisted sim state** (restored before `RenderRuntime` returns), so
  determinism holds and post-render consumers read the exact fixed-tick pose.
- Snapshot maps are keyed by `u32` (`std::to_underlying(entity)`), not `entt::entity` (no
  `std::hash`). Use `view.size()` for a single-component view — `.size_hint()` is only enabled for
  multi-component views, and getting this wrong is a real compile error. The `InterpTransform` struct
  is deliberately **not** named `*Component` so OloHeaderTool's scan never sweeps it up.

> **Smoothness is proven analytically, not by pixels.** A constant-velocity mover at 60 sim / 144
> display makes the interpolated render X exactly linear in wall time, so consecutive render deltas
> are constant (second difference ≈ 0) while the raw sim pose stair-steps. The change adds no new
> pixel-producing code — only different input transforms to an already-pixel-tested path.

## 6. SoundGraph parameter routing is opt-in per graph

`SoundGraphSound`'s `SetVolume`/`SetPitch`/`SetLooping`/`SetLowPassFilter`/`SetHighPassFilter` route
best-effort into the live graph as conventionally-named input parameters
(`Volume`/`Pitch`/`Loop`/`LowPass`/`HighPass`; `Loop` is a bool matching the `WavePlayer` node).

**There is no guaranteed parameter set.** A graph's exposed parameters are whatever the authored
asset declares as graph input streams, so a graph reacts only if it exposes a matching stream — and
a graph without the endpoint makes the write a silent no-op. Don't assume they exist.

A parameter handle is `static_cast<u32>(Identifier{endpointName})` — `Identifier`'s `u32` value **is**
the FNV hash of the name, and that is what `SendInputValue` matches. The accessor is
`GetParameters()`; there is **no** `GetParameterEndpoints()` (old stub comments referenced a
non-existent API).

> **Two incompatible parameter-ID spaces.** Graph endpoint IDs are FNV hashes. The
> `SoundGraphPatchPreset` "well-known" IDs (Volume=1, Pitch=2, LPF=10, HPF=11) used by the factory
> presets are **arbitrary small ints** and will not resolve against a real graph. A preset intended
> to drive a graph must register descriptors with `ID = static_cast<u32>(Identifier{endpointName})`.

**Device-free testing:** a `SoundGraphSource` works with no miniaudio device — never call
`Initialize(engine, …)`; `ReplaceGraph` + `ProcessSamples` drive it headless.
`SoundGraphSound::InitializeDetachedSource()` lets the wrapper be tested the same way.

## 7. Main→audio-thread handoff is an SPSC queue, not a mutex

`SoundGraphSource::ProcessSamples` runs on the miniaudio **audio thread** — no blocking locks, no
heap allocation.

The pattern: an SPSC `Audio::LockFreeEventQueue<T, 256>` carrying
`{ u32 m_ParameterID; Audio::PreAllocatedValue m_Value; }`. The **main thread** resolves the name to
an ID against the graph's exposed params, packs the scalar with `PreAllocatedValue::CopyFrom(...)`
and `Push()`es; the **audio thread** `Pop()`s in `ProcessSamples` and applies via `SendInputValue`.
`PreAllocatedValue::GetView()` aliases 64-byte inline storage, so reconstructing the view on the
audio thread allocates nothing.

This replaced a `ThreadSafePreset::GetPresetIfChanged` that deep-copied a whole preset (strings +
maps) under a mutex — calling that on the audio thread would lock *and* allocate.

> **Clear the queue when the validated-against context changes.** Each queued write was validated
> against the *current* graph's endpoints, so `ReplaceGraph` must `Clear()` after the swap (safe
> there, because the suspend protocol has the audio thread parked in the silence path) — otherwise a
> value validated against the old graph lands on a same-hash endpoint of the new one. Likewise, bulk
> apply must treat a **queue-full drop of a resolvable parameter as failure**, distinct from a
> descriptor that simply isn't a parameter of this graph.

Single live `SetParameter` writes still go straight to `SendInputValue`; only bulk preset application
is queued.

## 8. The spatializer hosts two upstream flavours

`Audio::DSP::Spatializer` (one shared instance) inserts a per-source VBAP-panning node between an
upstream node and its current downstream, via two `InitSource` overloads:

- **`ma_engine_node*`** — the `ma_sound`/`AudioSource` path. Carries a built-in `ma_spatializer`
  whose `dopplerPitch` the spatializer drives, so **Doppler works**.
- **`ma_node_base*`** — a **bare** custom node (e.g. `SoundGraphSource`, which attaches straight to
  the endpoint). No engine node, so the doppler sink is `nullptr` and **Doppler is skipped**; VBAP
  panning and distance/cone attenuation run identically (they are CPU geometry inside the node).

> **The crux gotcha:** `ma_node` is `typedef void` in miniaudio, but `ma_engine_node` and
> `ma_node_base` are **distinct concrete structs** — `ma_engine_node` has
> `.resampler`/`.spatializer`/`.baseNode` that a bare node lacks. Any code reaching into those must
> null-guard the bare-node case.

Before #424 the whole Spatializer was built but wired to **nothing** in production — `AudioSource`
uses miniaudio's built-in `ma_sound` spatialization and nobody called `InitSource`.
`SoundGraphSource` is the first real consumer. Spatialization for SoundGraph voices is a
**runtime-only flag**, deliberately not a serialized field (that would force a serializer edit).

**Functional tests can't exercise this:** `FunctionalTest::EnableAudio` calls
`Scene::InitAudioRuntime` but **not** `AudioEngine::Init`, so there is no live `ma_engine`. Test with
a **device-free** `ma_engine` (`config.noDevice = MA_TRUE`, plus channels/sampleRate/listenerCount) —
node graph and resource manager exist, no hardware opened.

> **Leak gotcha:** `InitSource` allocates **three** things `ReleaseSource` must free — the `ma_node`,
> the `Scope<VBAPData>`, **and** the `ma_channel_converter`. The converter free was missing (a latent
> ~72 B/source leak) and only ever showed on the **Linux/Clang ASan+LSan** job; Windows/MSVC doesn't
> run LeakSanitizer, so it passed locally. Mirror every `ma_*_init` with its `_uninit`.

## 9. Two SoundGraph caches with distinct roles

- **`SoundGraphCache` is purely in-memory** — an LRU cache of live compiled `Ref<SoundGraph>`
  instances with **no on-disk format of its own**. A branch once added bespoke YAML persistence here
  and it was removed in review as a duplicate of `CompilerCache`. **Don't re-add it.**
- **`CompilerCache` owns cross-run persistence** — path, hash, timestamps **plus the compiled
  bytecode**, a superset of anything the other could record. Route any "skip recompilation across
  runs" need through here. It uses its own binary per-file format (the subsystem's convention
  elsewhere is YAML).
- The compiled `Ref<SoundGraph>` is runtime-only and never serialized; only the bytecode is.
- **Per-node memory accounting uses a virtual hook, not a `dynamic_cast`.**
  `NodeProcessor::GetHeapBytes()` returns 0 by default and `WavePlayer` overrides it, so new
  buffer-owning node types are counted automatically (this replaced a flat 2 MB/node guess).

## 10. Authoring `.olo` scenes programmatically

- **BoxCollider3D `HalfExtents` are MESH-LOCAL** — JoltShapes multiplies them by the transform scale.
  A unit cube scaled to `[160,1,160]` needs HalfExtents `[0.5,0.5,0.5]`, **not** `[80,0.5,80]`, or
  `ValidateBoxDimensions` fails and the engine **silently substitutes a default shape**. The
  checked-in `Physics3DTest.olo` ground has this wrong (Scale 50 × Half 25).
- **Edit mode renders the EDITOR camera**, not the scene's `CameraComponent` — pose it via
  `olo_camera_set_pose`. Physics, scripts and animation do **not** tick in Edit mode, and
  **world-space `SpriteRendererComponent`s render only on the runtime path**. Any such scene must run
  in Play mode.
- **`AnimationStateComponent::SourceFilePath` is relative to the project Assets dir**
  (`../../assets/models/Fox/Fox.gltf` reaches `OloEditor/assets`). Deserialize does a full
  `Ref<AnimatedModel>::Create` **per entity** — no sharing.
- **Verify the workload actually runs.** A swarm scene that silently fails reads as a great fps
  number; probe a designated entity's Translation twice.
- **Asset-orphan CI checks bite generator-only scripts.** A `.cs`/`.lua` referenced only by
  git-ignored generated scenes is invisible to CI and fails the "every script is referenced by a
  scene" tests; a `.cs` additionally needs an `AssetRegistry.oar` entry. Fix with a tiny **committed**
  scene outside the ignored dir that references the script.
- **Limits worth knowing:** Jolt MaxBodies 65536, MaxBodyPairs 65536, **MaxContactConstraints
  10240** (a 10k-body pile overflows → `Jolt physics update error: 4`). FrameDataBuffer per frame:
  **4096 bone matrices** (~170 24-bone characters), **16384 instance colors/entityIDs**, **1024 unique
  materials**. Overflow errors are **unthrottled** — 100k+ log lines can stall the editor main thread.

## 11. Frame pacing needs 1 ms timer resolution on Windows

`Core/FramePacer.{h,cpp}` is owned by `Application` and driven from `Run()` — `LimitFrameRate` is
called **after** `SwapBuffers` so it composes with vsync.

The default Windows scheduler tick is ~15.6 ms, so a limiter is useless without raising the process
timer resolution: `FramePacer` calls `timeBeginPeriod(1)` while a cap is active and `timeEndPeriod(1)`
when it is turned off or destroyed.

**Any future sleep-based pacing must pair a coarse `sleep(remaining - margin)` with a short busy-spin
for the final ~1 ms** — the OS sleep alone overshoots. And **guard the spin against a frozen clock**
(`Time::HasMockTime()`) or deterministic captures and tests hang forever.
