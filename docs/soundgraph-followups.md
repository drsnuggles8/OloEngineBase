# SoundGraph follow-ups

Reviewer findings that landed during the `feature/soundgraph-runtime` PR but
were deliberately deferred. Each is real, with a concrete fix sketched; none
is a blocker for the runtime as it stands, and folding them in would have
expanded the PR past where it was useful to review.

Companion: [docs/soundgraph-metasounds-refactor.md](soundgraph-metasounds-refactor.md)
holds the larger architectural plan (block-based execution, typed
connections, flat operator list).

---

## 1. `AudioSoundGraphComponent::SoundGraphHandle` exposed to scripting

**State:** the C# binding for `AudioSoundGraphComponent` exposes
`Volume` / `Pitch` / `Looping` / `PlayOnAwake` via `OLO_PROPERTY`, but not the
`SoundGraphHandle` asset reference.

**Why deferred:** `OloHeaderTool`'s type mapper
([tools/OloHeaderTool/main.cpp:58-77](../tools/OloHeaderTool/main.cpp#L58-L77))
only supports `float`/`bool`/`int`/`uint`/`vec2-4`/`string`. There's no
`u64`/`AssetHandle` mapping. `AudioSourceComponent` follows the same pattern
(its audio clip handle has no `OLO_PROPERTY` either — set by the editor, not
scripts).

**Fix sketch:** extend `OloHeaderTool` to handle `AssetHandle`/`u64`, mapping
them to `ulong` on the C# side. Wire that into `Components.h` for both
`AudioSoundGraphComponent` and `AudioSourceComponent`.

---

## 2. `AudioSoundGraphComponent::operator==` bitwise float comparison

**State:** the equality operator uses direct `==` on `VolumeMultiplier` and
`PitchMultiplier`. Project convention is to avoid float `==` (see
[docs/agent-rules/cpp-coding-quality.md §2](agent-rules/cpp-coding-quality.md)),
which the reviewer correctly flagged.

**Why deferred:** the reviewer's suggested fix referenced "the repository's
bitwise float comparison helper", but no such helper exists. Bare `==` is
bit-exact except for `NaN`, and the boundary now drops `NaN` via
`SoundGraphSerializer` (`std::isfinite` guards on PosX/PosY land in this PR);
asset properties on the gameplay-facing component never go through
deserialisation, so `NaN`/`Inf` can't reach them in practice.

**Fix sketch:** add a `BitwiseEqual(f32, f32)` (or `std::bit_cast<u32>`)
helper to `Core/Math` and use it across every component `operator==` that
compares floats. Many other components in `Components.h` have the same shape,
so this is a sweep, not a one-line fix.

---

## 3. `AnimationGraphEditorPanel` no-op undo entries

**State:** `BeginEditSession` always takes a snapshot, and `EndEditSession`
flushes it once `GImGui->ActiveId == 0`. If a session is bracketed without
any actual mutation, an indistinguishable undo entry still lands on the
history stack.

**Why deferred:** the proper fix needs either:

- an `operator==` (or structural diff) on `AnimationGraph` — doesn't exist,
  and the graph carries skeleton/animation refs that aren't trivially
  comparable; or
- restructuring every Begin/End call site (there are many — see the grep at
  the time of the PR) to gate on widget-activation transitions instead of
  taking the snapshot eagerly.

Either path is a sizeable refactor of the panel and out of scope for what
the rest of the PR addresses (sound graph runtime, not animation graph
editor).

**Fix sketch:** add `AnimationGraph::Hash()` (or canonicalise to YAML and
compare the bytes). In `EndEditSession`, only call `PushSnapshot` when the
hash differs from `m_EditSessionSnapshot`. Drop the unconditional `Begin` at
the top of the panel render and gate it on `GImGui->ActiveId != 0`.

---

## 4. `WavePlayer::Tasks::Launch` lambda lifetime / UAF risk

**State:** [`WavePlayer::StartAsyncLoad`](../OloEngine/src/OloEngine/Audio/SoundGraph/Nodes/WavePlayer.h)
launches a background task whose lambda captures raw `this`. The destructor
sets `m_ShuttingDown.store(true)` and cancels in-flight loads via
`m_LoadGeneration.fetch_add`, but it doesn't wait for the task to finish.
A task that's already past its `m_ShuttingDown.load()` gate can still touch
member atomics (and briefly the `m_LoadResultMutex`) after `~WavePlayer`
runs.

**Why deferred:** the cleanest fix is having `WavePlayer` inherit
`std::enable_shared_from_this` and capture a `std::weak_ptr<WavePlayer>` in
the lambda. That cascades into every owner of `WavePlayer` — currently
`std::vector<Scope<NodeProcessor>>` in `SoundGraph` (a `unique_ptr` analogue)
— having to switch to `shared_ptr`-style storage. That's an ownership-model
change for every node type, not just `WavePlayer`, and was too invasive to
bundle with the rest of the PR.

The current window is narrow: the lambda's only post-gate accesses are
atomic loads + a brief mutex critical section. Hitting it requires destroying
a `WavePlayer` while a load is mid-flight — which only happens on graph
hot-reload during an active asset load, and the destructor's
`m_LoadGeneration.fetch_add` already invalidates the load before destruction
proceeds.

**Fix sketch:** two options, in increasing order of scope:

1. Have the destructor join the task. Track the `TTask<void>` handle returned
   by `Tasks::Launch` in a `std::optional` member, wait on it (with a
   timeout) in `~WavePlayer`. Cheap, contained, but blocks the destructor.
2. Switch `NodeProcessor` storage to `shared_ptr` (or a custom intrusive
   ref-counted handle), make `WavePlayer` inherit
   `std::enable_shared_from_this`, capture `weak_from_this()` in the lambda,
   and have the lambda early-return if `lock()` fails.

Pick depends on the broader question of how the SoundGraph runtime models
node ownership — covered by Phase 3 of the
[MetaSounds refactor plan](soundgraph-metasounds-refactor.md).

---

## 5. `Scene::OnComponentAdded<AudioSoundGraphComponent>` runtime init

**State:** the `OnComponentAdded<AudioSoundGraphComponent>` hook is a no-op,
so components added during runtime (script-spawned entities, networked actors
arriving mid-session, etc.) never get their `Sound` ref initialised and stay
silent until next `InitAudioRuntime`.

**Why deferred:** the fix requires factoring the per-component startup body
inside `Scene::InitAudioRuntime`
([Scene.cpp:499-566](../OloEngine/src/OloEngine/Scene/Scene.cpp#L499))
into a reusable helper (e.g. `InitializeAudioSoundGraph(sgc)`) and calling
it from both places. That's straightforward, but the same gap exists for
`Rigidbody3DComponent` and other runtime-spawned components — the broader
question is whether `OnComponentAdded` should always be the canonical
"initialise me" entry point and whether `InitAudioRuntime` is then just "fire
OnComponentAdded for every existing component". Worth solving once across
the engine, not per-subsystem.

**Fix sketch:** extract `Scene::InitializeAudioSoundGraph(AudioSoundGraphComponent&)`
from the loop body, call it from both `InitAudioRuntime` and
`OnComponentAdded<AudioSoundGraphComponent>` (gated on `m_IsRunning` to
preserve edit-mode semantics).

---

## Linkage

The PR description lists these inline so reviewers see them without leaving
the PR; this doc is the long-form home so they survive past PR merge. When
one gets picked up, link the resolving PR back to the relevant section
header so this doc shrinks over time.
