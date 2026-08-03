# Audio voice budget — stealing and virtualization (issue #730)

`Audio/VoiceManager.{h,cpp}` caps the number of concurrently *audible* voices, ranks the
contenders, and keeps the losers running silently so they can come back mid-stream. Read
this before touching anything that starts, stops, or scores a sound.

---

## 1. There is no such thing as "the" playback call site — put the budget inside `Play()`

The engine starts sounds from at least six places: `Scene::InitAudioRuntime` (the
`PlayOnAwake` loop), `Scene::InitializeAudioSoundGraph`, `SceneStreamer` (a streamed-in
chunk resuming its ambience), `AudioEventsManager::Update` (the event/trigger system
creating a source on the fly), `ScriptGlue.cpp` (C#), and `LuaScriptGlue.cpp`. The
handover for #730 named two of them; a grep for `->Play()` found six.

A budget wired into call sites is a budget with holes, and the hole is invisible: a test
that drives the component path passes while the script path and the event path bypass the
cap entirely. **The admission point is `AudioSource::Play()` and
`SoundGraphSound::Play()` themselves** — every caller reaches the budget for free, and a
seventh call site added tomorrow is covered without anyone remembering this document.

The corollary is that `Play()` no longer means "this is now audible". It means "this voice
is now registered and will be audible when it is worth a slot". Anything that treats
`Play()` as a guarantee of output is wrong.

## 2. The policy object must not know about the backend

`VoiceManager` is a pure decision unit over `VoiceParams` (priority, gain, position,
looping, pitch, duration) driven through the `IVoiceHost` callback interface. It contains
no miniaudio, no `ma_sound`, no engine handle.

That is not stylistic. **A real `AudioSource` cannot be constructed in a headless unit
test** — its constructor calls `ma_sound_init_from_file` against a live `ma_engine` that
only exists after `AudioEngine::Init` opened a device. The same constraint bit the #607
MCP slice (see `mcp-setter-based-field-registry.md`). A budget implemented inside
`AudioSource` would have been untestable in CI, which for a stability-weighted feature is
the whole value gone. Design the policy as a separate object *first*, then adapt the
backends to it — don't discover this when you sit down to write the test.

`OloEngine/tests/Audio/VoiceManagerTest.cpp` exercises every rule below with a recording
`FakeVoiceHost` and no audio device.

## 3. "It's playing again" is not the assertion — the *position* is

The subtle half of virtualization. When a stolen looping ambience is later given its slot
back, three behaviours all satisfy "is it playing?":

1. restart from zero — **wrong**, audibly jumps, and two copies of the same loop drift apart;
2. resume at the position it was stolen at — **wrong**, the loop is now behind by however
   long it was silent;
3. resume at the position it *would* have reached had it never stopped — correct.

Only (3) is right, and only (3) needs the manager to keep advancing a logical playback
position (`position += dt * pitch`, wrapped modulo the clip length) for voices that are
not playing. Assert `LastStartPosition`, not `IsPlaying()`.

**But resuming is only right for a LOOP.** Issue #730 says a sound that cannot win a slot
"is refused", and the same seek that puts a loop back in phase would put a one-shot
somewhere in its tail — emitting the last 100 ms of an impact the player never heard
begin. `CanBecomeAudible` is the rule, and its clauses are in priority order:

1. **paused** → never (see §6 — owner intent outranks the budget);
2. **was ever audible** → always, because the player heard this sound begin, so bringing
   it back continues something already in their ear;
3. **looping** → always (it resumes in phase, which is the whole point);
4. **unknown length** (a stream, a SoundGraph voice) → always, because we cannot schedule
   its end and refusing it forever would strand it silent rather than merely skip it;
5. otherwise → only from position 0.

The real distinction is **"would starting now drop the player into the middle of a sound
they never heard start?"** — not "is it a one-shot". A fresh one-shot has missed nothing
and must play; a one-shot that was audible and got stolen may resume; only one that has
*only ever advanced silently* is refused.

Corollary for the *audible* case: while a voice really is running, the **backend cursor is
authoritative** (`OnVoiceQueryPosition`). Integrating `dt` for an audible voice drifts
against the device clock, and the drift only becomes visible much later, as a wrong resume
phase after a steal that happens minutes in.

## 4. Hysteresis is a correctness requirement, not a polish item

Two voices scoring 0.500 and 0.501 with a full mix will trade the slot on *every tick*
without a promotion margin — a stop/start per frame, which is an audible stutter and a
pile of backend churn. `kDefaultPromotionMargin` (0.02) is what stops it: a virtual voice
takes an audible one's slot only by beating it by more than the margin.

The margin is also what guarantees the steal loop terminates: each swap raises the total
audible score by at least the margin, and the score is bounded. The explicit iteration cap
in `Rebalance` exists only for a deliberately-zero margin.

## 5. Never call a host with the lock held

`VoiceManager` takes a mutex on every public method — it must, because
`Scene::UpdateAudio` is `.Parallelizable()` in the gameplay scheduler (it runs on a worker
thread) while scripts start sounds from the game thread.

`OnVoiceStart` / `OnVoiceStop` are therefore **queued as `PendingTransition`s under the
data lock and applied after it is released** — a backend call under the data lock would
hold it for the length of a `ma_sound_start`.

There are **two** locks, and the ordering is load-bearing:

- `m_TransitionMutex` — held across "decide the transitions, then apply them" as one unit.
  Without it, two threads can each compute a self-consistent batch, drop the data lock,
  and then interleave their backend calls, leaving a host stopped while its record says
  `Playing`. The data structure stays consistent; the *device* does not.
- `m_Mutex` — the voice table, held only for short callback-free stretches.

Always transition-then-data. Nothing may take the data lock first and then the transition
lock. The cost is that **no `IVoiceHost` callback may re-enter the manager's mutating
API** — `OnVoiceStart`/`OnVoiceStop` run with the transition lock held, so calling
`Acquire`/`Release`/`Update`/`SetMaxVoices` from one self-deadlocks. `OnVoiceQueryPosition`
runs under the data lock and must not re-enter either.

## 6. Owner intent outranks budget policy — the pause trap

A paused voice is silent, so the obvious move is to let the budget reclaim its slot. The
non-obvious half: if a paused voice stays *promotable*, the budget will eventually call
`OnVoiceStart` on it and **resume playback the game explicitly stopped**. Pause is an owner
decision; the budget may take a paused voice's slot away, but must never give it back.

`VoiceParams::Paused` therefore does two things: it scores the voice zero (so any voice
that can actually be heard outbids it), and it makes `CanBecomeAudible` return false (so
only the owner's `SetVoicePaused(handle, false)` can revive it).

Two consequences worth knowing:

- **An uncontended pause keeps its slot.** Scoring zero only matters when someone wants
  the slot; with no contender there is nobody to hand it to, and stopping the backend for
  no gain is pure churn. The budget self-corrects the instant a real contender arrives.
- **Pause/resume is why `WasAudible` exists.** §3's refusal rule ("a one-shot that only
  ever advanced silently must not be seeked into") would otherwise refuse a one-shot that
  the player *did* hear, was paused mid-way, and then un-paused. The rule keys on whether
  the voice was ever audible, not on how far it has advanced.

## 7. A slot is leaked unless something retires the voice

Owners are not required to call `Stop()` on a sound that simply ended, so the manager
retires a non-looping voice itself once its logical position passes `DurationSeconds`.
Without that, every completed one-shot would hold a slot forever and the budget would silt
up until nothing new was ever audible.

The escape hatch is `DurationSeconds == 0` = "unknown length" (a stream, a SoundGraph
voice): such a voice is **never** auto-retired and its owner must `Release` it — which for
`AudioSource` / `SoundGraphSound` happens in `Stop()` *and* in the destructor, because the
manager holds a raw `IVoiceHost*` and must never drive a torn-down host.

## 8. The two backends virtualize differently, and one of them can't do it properly

| | clip path (`AudioSource`) | graph path (`SoundGraphSound`) |
|---|---|---|
| virtualize | `ma_sound_stop` — frees mixer *and* DSP cost | mute the graph's `Volume` input |
| devirtualize | `ma_sound_seek_to_pcm_frame(position)` + start | un-mute |
| resume phase | exact (sample cursor) | exact for free (the graph never stopped) |
| reclaims CPU | yes | **no** |

`SoundGraphSource` exposes `SendPlayEvent()` and nothing to seek or suspend with, so a
virtualized graph voice is silenced rather than suspended. That still holds the audible cap
— which is the acceptance criterion — but it does not reclaim the DSP cost. Fixing that
needs a stop/seek API on `SoundGraphSource` first (tracked as **#745**, which also raises
the harder question of whether the graph's stateful nodes can be resumed deterministically
at all); don't "fix" it by stopping the graph, which would restart it from its initial
state on devirtualization and reintroduce §3's bug.

Watch the polarity trap while you are in there: `SoundGraphSound::m_Priority` is
miniaudio-flavoured (**0 = highest**) while `VoiceParams::Priority` is the other way round
(**1 = highest**). `BuildVoiceParams` inverts. Getting that backwards makes the most
important sounds the first ones stolen, and every test that only counts voices still
passes.

## 9. Adding a field to `AudioSourceConfig` is four edits, and one of them is silent

`AudioSourceComponent` is **not** an auto-generated-serializer component (its fields live
behind a private `m_Cold` blob, which the generator classifies non-trivial and skips), so
`Priority` needed:

1. `Audio/AudioSource.h` — the field on `AudioSourceConfig`.
2. `Scene/SceneSerializer.cpp` — **both** the hand-written serialize and deserialize
   blocks, with the `TrySetDsp` clamp; a missing key keeps the default.
3. `SaveGame/SaveGameComponentSerializer.cpp` — the `Serialize()` overload. **Silent if
   forgotten**: the field round-trips through scene YAML while being dropped from every
   save-game. The archive is fixed-order, so the new field is appended *last* and gated
   behind `HasFieldsSince(ar, 14)` with a matching `kSaveGameFormatVersion` bump — an
   ungated read consumes the next component's bytes out of every older save
   (`binary-format-versioning.md`).
4. `Scene/Components.h` — the `OLO_PROPERTY` annotation, which is what makes it
   MCP-writable and script-visible.

Also note `static_assert(sizeof(AudioSourceComponent) <= 32)`: new authored audio state
belongs in `AudioSourceColdData` / `AudioSourceConfig`, never inline on the component.

## 10. Re-score on every input that moves

The score is `Priority × gain × distanceAttenuation`, so **every** mutator feeding those
inputs has to push fresh params at the manager — `SetVolume`, `SetPitch`, `SetLooping`,
`SetPosition`, `SetMinDistance` / `SetMaxDistance`, `SetSpatialization`, `SetPriority`, and
`SetConfig`. Miss `SetPosition` and distance-based stealing silently ranks every voice on
where it was when it started; the mix still sounds plausible, and no test that never moves
an emitter will notice.

`Scene::UpdateAudio` ticks the manager **last**, after the listener sync, the per-source
position sync, and `AudioEventsManager::Update` — so the rebalance sees this frame's
spatial state and this frame's newly triggered sounds, not the previous frame's.
