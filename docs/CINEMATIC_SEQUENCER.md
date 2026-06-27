# Cinematic Sequencer

Timeline-based playback of authored cutscenes (issue #177). A
`CinematicSequence` drives scene entities over time — camera moves,
entity transforms, visibility toggles, and timed events — and is played
per-entity through a `CinematicComponent` that `CinematicSystem` advances
every runtime tick.

This document covers **what shipped** (a complete, tested runtime +
serialization slice with a component inspector) and **what's deliberately
deferred**. It is not a duplicate of the source comments — read the headers
under [`OloEngine/src/OloEngine/Cinematic/`](../OloEngine/src/OloEngine/Cinematic/)
for the per-type contracts.

---

## Try it in the editor

A self-contained demo ships under the SandboxProject (colored sprites + a
camera, **no mesh/material assets required**):

- Sequence: [`Assets/Cinematics/IntroSequence.olocine`](../OloEditor/SandboxProject/Assets/Cinematics/IntroSequence.olocine)
- Scene: [`Assets/Scenes/CinematicDemo.olo`](../OloEditor/SandboxProject/Assets/Scenes/CinematicDemo.olo)

Steps (run `OloEditor`, working dir `OloEditor/`):

1. Open **CinematicDemo.olo**. It has a camera, a cyan *MovingSprite*, a
   magenta backdrop, and a *Director* entity carrying a `CinematicComponent`
   (PlayOnStart + Loop already ticked).
2. Select **Director**. In its *Cinematic Sequence* component, drag
   `IntroSequence.olocine` from the Content Browser onto the **Sequence** slot.
   (The `.olocine` is auto-imported on project load, so it's already in the
   browser. The sequence targets the demo entities by UUID, so it only drives
   *this* scene's camera + sprite.)
3. **Preview in edit mode** (no Play mode needed): drag the *Scrub* slider to
   pose the scene at any time, or press the inspector's **Play** button — it
   runs a live preview using the editor frame clock *while the Director stays
   selected* (the runtime cinematic tick only runs in Play mode, so the
   inspector drives the edit-mode preview itself). The sprite eases across and
   pulses; the camera entity pans.
4. **Or enter Play mode** (editor toolbar ▶): `PlayOnStart` auto-runs the
   cutscene through the real runtime tick — the camera sweeps left + zooms, the
   sprite animates, and the `begin` / `midpoint` events log to `OloEngine.log`.
   It loops.

To use it on your own scene: add a **Cinematic Sequence** component to an
entity (Add Component ▸ Cinematic Sequence), author a `.olocine` (see the demo
for the YAML shape) whose track `Target` UUIDs match your entities, and assign
it.

> The exact import → handle-resolve → playback path the demo relies on is
> pinned headlessly by `CinematicAssetPlaybackTest` (it imports the shipped
> `.olocine` and asserts it drives the demo UUIDs), so the walkthrough can't
> silently rot even though the GUI itself isn't auto-tested.

---

## Architecture

```text
CinematicCurve.{h,cpp}        Float / Vec3 / Quat keyframe channels + interp modes
CinematicTrack.h              Transform / Camera / Visibility / Event track structs
CinematicSequence.{h,cpp}     Asset (AssetType::CinematicSequence) holding the tracks
CinematicComponent.h          ECS component: handle + playback flags + runtime playhead
CinematicPlayer.{h,cpp}       Pure playback math (advance, loop/clamp, event edges)
CinematicSystem.{h,cpp}       Per-entity tick + apply-to-scene, run from OnUpdateRuntime
CinematicSequenceSerializer.{h,cpp}   `.olocine` YAML round-trip
```

The split is deliberate: **`CinematicPlayer` is pure** (plain time values
in, advanced playhead + fired events out — no Scene, no GL), so the
timeline semantics are unit-testable; **`CinematicSystem`** owns the
per-entity bookkeeping and the UUID→Entity application. A sequence is an
**asset** (reusable, referenced by handle), mirroring `AnimationGraphAsset`;
the component is lightweight and resolves the sequence lazily (or accepts a
directly-assigned `RuntimeSequence`, which is how tests drive it without an
asset file).

### Tracks

| Track | Target | Drives | Interpolation |
|-------|--------|--------|---------------|
| `CinematicTransformTrack` | any entity w/ `TransformComponent` | translation / rotation / scale | per-key (Constant / Linear / EaseInOut / Bezier; rotation = slerp) |
| `CinematicCameraTrack` | entity w/ `Transform` + `CameraComponent` | position, rotation, vertical FOV | as above |
| `CinematicVisibilityTrack` | entity w/ `ModelComponent` | `m_Visible` | step (latest key ≤ playhead wins) |
| `CinematicEventTrack` | — | named events at timestamps | edge-triggered |

Only channels that actually carry keys are applied, so a track can drive a
single property without clobbering the others. Events fire exactly once as
the playhead crosses each key (the player tracks a half-open `(floor, now]`
window and handles loop wrap by firing the old lap's tail then the new
lap's head). Fired event names land in `CinematicComponent::EventsFiredThisFrame`
each tick.

### Playback

`CinematicComponent` carries serialized authoring data (`Sequence` handle,
`PlayOnStart`, `Loop`, `PlaybackSpeed`) plus runtime-only playhead state
that is reset on copy (so scene-copy / prefab instances get their own
playback, mirroring `AnimationGraphComponent::RuntimeGraph`). Lua scripts
get `Play / PlayFromStart / Pause / Stop` plus `loop`, `playbackSpeed`, and
read-only `isPlaying / time / isFinished`.

`CinematicSystem::Update` runs in `Scene::OnUpdateRuntime` **after scripts**
(so a playing cutscene's authored motion wins over script motion for the
frame) and **before** dialogue / animation / physics.

### Serialization & cross-bindings

`.olocine` is YAML (`CinematicSequenceSerializer`), wired into the asset
pipeline like every other asset type: `AssetType::CinematicSequence`,
extension map, `CinematicSequenceAssetSerializer` registered in
`AssetImporter`. The `CinematicComponent` updates all six component
touch-points (`AllComponents`, `Scene::OnComponentAdded`/`OnComponentRemoved`
specializations in `Scene.cpp`, `SceneSerializer`, `SaveGameComponentSerializer`,
`OLO_PROPERTY` annotations, `LuaScriptGlue`) so scenes and save-games
round-trip it. All floats read back from YAML / save-games are validated
with `std::isfinite`.

### Tests

- `CinematicCurveTest` (unit) — channel interpolation, endpoint clamping,
  the four interp modes (incl. the Bezier tangent ease and its
  smoothstep/linear anchor identities), quat slerp/normalization, degenerate-
  segment safety, visibility step semantics.
- `CinematicPlayerTest` (unit) — time advance (forward / reverse / clamp /
  finish-at-0 / loop-wrap both directions / zero-duration / zero-speed hold /
  forward-backward symmetry), half-open event windows (ascending and the
  reverse descending mirror), and the composite `Tick` (t==0 firing,
  no double-fire, loop wrap forward and backward).
- `CinematicSerializerTest` (unit) — full `.olocine` round-trip across every
  track/channel + malformed-input guard + Bezier tangent round-trip and the v1
  back-compat path (legacy files with no tangent fields load flat).
- `CinematicDrivesEntitiesTest` (Functional) — drives a real `Scene` through
  `OnUpdateRuntime` and asserts the sequence poses entities, fires its event,
  finishes (non-looping) and never finishes / re-fires t==0 (looping).

---

## Editor timeline panel — **shipped**

[`CinematicTimelinePanel`](../OloEditor/src/Panels/CinematicTimelinePanel.h) is the
authoring surface on top of the runtime. Open a `.olocine` by double-clicking it in
the Content Browser, or via **Edit in Timeline** on the `CinematicComponent`
inspector (alongside the existing assign / Loop / PlayOnStart / speed / Scrub
controls). Toggle it from **View ▸ Cinematic Timeline**.

What it does:

- **Track lanes** — every track expands into its editable channels (Transform →
  Translation / Rotation / Scale; Camera → Pos / Rot / FOV; plus Visibility and
  Event lanes), drawn against a time ruler with zoom (Ctrl+wheel) and pan (wheel).
- **Keyframes** — diamonds coloured by interpolation mode. **Drag** to retime,
  **double-click** an empty lane (or the lane's **＋**) to insert a key at that
  time, snapshotting the target entity's current value. **Snap** to a configurable
  grid. Right-panel inspector edits the selected key's time, value (vec3 / euler /
  FOV-degrees / visible / event-name) and interpolation; **Delete Key** removes it.
  Picking **Bezier** reveals the key's **In / Out tangent** handles (numeric drags).
- **Curve preview** — the inspector plots the channel's *evaluated* output across
  the sequence, so the chosen interp modes are visible (Constant = steps, Linear =
  straight, EaseInOut = smooth, Bezier = the tangent-shaped ease — drag the handles
  and watch the curve flex live).
- **Playhead** — drag the ruler to scrub (poses the scene live through
  `CinematicSystem::ApplyAtTime`), or **Play/Pause/Stop** for an edit-mode preview
  (Loop optional). The explicit **Duration** field (0 = auto) and **Add Track**
  (with an entity-target picker) round it out.
- **Save** writes back through `CinematicSequenceSerializer`. Editing is in-place
  on the AssetManager's cached `Ref<CinematicSequence>`, so a referencing
  `CinematicComponent` (and the inspector scrub slider) sees edits immediately.

All key mutations route through the pure, unit-tested `OloEngine::CinematicEdit`
helpers (insert-sorted / move-and-resort / remove), so the sort-by-time invariant
that playback depends on is enforced in one place
([`CinematicEditTest`](../OloEngine/tests/Cinematic/CinematicEditTest.cpp)). The
panel itself is editor UI and isn't auto-tested.

### Bezier tangents — **shipped**

`CinematicInterp::Bezier` adds per-key **in/out tangent handles** evaluated as a
cubic-Hermite ease. Each tangent is a *slope of the normalized 0→1 segment ease*:
a Bezier segment leaving `key[i]` reads `key[i].OutTangent` and `key[i+1].InTangent`.
The anchor identities make it a clean superset of the older modes — **(0, 0)
reproduces EaseInOut** (smoothstep), so a fresh Bezier key matches prior behaviour
until a handle moves, and **(1, 1) reproduces Linear**. Larger tangents give
anticipation / overshoot; the shaped blend is applied uniformly to the value the
same way the other modes are, so tangents control a segment's *timing/ease* (and
overshoot for float & vec3) — the quaternion ease is clamped to [0,1] before slerp
so rotations never whip past their endpoints. Persisted in `.olocine` **v2** as two
extra floats per key; older v1 files (no tangent fields) load unchanged with flat
(0) tangents. Math pinned by `CinematicCurveTest`, round-trip + back-compat by
`CinematicSerializerTest`.

> What's *not* covered: free **value-space** tangents (a curved spatial path
> between two position keys, or independent per-axis overshoot). The current model
> shapes the segment's easing uniformly across components — the path between two
> keys stays a straight line, only the speed along it curves. Per-component value
> splines would be a larger change to the channel evaluation (each component would
> need its own tangent, and quaternions a squad-style scheme) and are left open.

### Additional track types

The issue lists Audio, Dialogue, PostProcess, and Subtitle tracks. They are
omitted from this slice because each couples to another subsystem's runtime
(AudioEventsManager, DialogueSystem, PostProcessSettings, the UI layer) and
is better added once that seam is needed. The track model is open for
extension — add a struct in `CinematicTrack.h`, a `std::vector` on
`CinematicSequence`, an apply branch in `CinematicSystem`, and serializer
coverage. EventTracks already provide a generic escape hatch (fire a named
event, handle it in script) for cases that don't yet have a dedicated track.

### Variable-rate playback

`PlaybackSpeed` is a free time scale: positive plays forward, **negative
plays the sequence backward**, and `0` holds the playhead (a live "pause"
without clearing `Playing`). It round-trips through both scene YAML and
save-games — the deserializers accept any finite value (the save-game path
clamps to the inspector's symmetric `[-16, 16]` authoring range). Reverse
playback mirrors the forward semantics — the playhead recedes toward `0`,
finishes at `0` when not looping, and wraps `0 -> duration` when looping.
Event firing flips to the descending crossing
(`CinematicPlayer::CollectEventsReverse`, the `[lo, hi)` mirror of the
forward `(lo, hi]` window), so cues fire in the order a receding playhead
reaches them and never double-fire across a direction change.

**Direction-aware start.** `Play()` / `PlayFromStart()` / `PlayOnStart` /
`Stop()` all leave the playhead at the *forward* start (`Time = 0`), which is
the *finish* line for reverse. `CinematicSystem::Advance` detects a fresh
negative-speed start (the `PreviousTime = -1` sentinel still set) and seeds
the playhead to `duration` on the first tick, so a backward sequence plays the
whole timeline instead of finishing instantly at `0`. Resuming after a pause
keeps a real `PreviousTime >= 0` and is left untouched.

One asymmetry worth knowing: just as scrubbing/`PlayFromStart` to `t == 0`
and playing forward only fires the `t == 0` cue via the `PreviousTime = -1`
sentinel, there is no symmetric "start from the end" sentinel — the reverse
seed sets `PreviousTime = duration`, so a cue authored at exactly
`t == duration` is *not* fired on the first backward step (the playhead leaves
it without crossing it). Variable *rate* beyond a constant scale (ease-in/out
of the time scale) is still future work.

### Sequence blending & layering

A single component plays one sequence at a time. Blending between sequences
(or layering an additive camera-shake track over a base move) is a larger
design and isn't required for the cutscene use case this slice targets.
