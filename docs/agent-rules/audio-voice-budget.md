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
lock and applied after it is released**. A host is entitled to call back into the manager
from them (`AudioSource::OnVoiceStart` does not today, but nothing stops one), and a
straight call under the lock would self-deadlock. The one exception is
`OnVoiceQueryPosition`, which *is* called under the lock and is documented as
must-not-re-enter.

## 6. A slot is leaked unless something retires the voice

Owners are not required to call `Stop()` on a sound that simply ended, so the manager
retires a non-looping voice itself once its logical position passes `DurationSeconds`.
Without that, every completed one-shot would hold a slot forever and the budget would silt
up until nothing new was ever audible.

The escape hatch is `DurationSeconds == 0` = "unknown length" (a stream, a SoundGraph
voice): such a voice is **never** auto-retired and its owner must `Release` it — which for
`AudioSource` / `SoundGraphSound` happens in `Stop()` *and* in the destructor, because the
manager holds a raw `IVoiceHost*` and must never drive a torn-down host.

## 7. The two backends virtualize differently, and one of them can't do it properly

| | clip path (`AudioSource`) | graph path (`SoundGraphSound`) |
|---|---|---|
| virtualize | `ma_sound_stop` — frees mixer *and* DSP cost | mute the graph's `Volume` input |
| devirtualize | `ma_sound_seek_to_pcm_frame(position)` + start | un-mute |
| resume phase | exact (sample cursor) | exact for free (the graph never stopped) |
| reclaims CPU | yes | **no** |

`SoundGraphSource` exposes `SendPlayEvent()` and nothing to seek or suspend with, so a
virtualized graph voice is silenced rather than suspended. That still holds the audible cap
— which is the acceptance criterion — but it does not reclaim the DSP cost. Fixing that
needs a stop/seek API on `SoundGraphSource` first; don't "fix" it by stopping the graph,
which would restart it from its initial state on devirtualization and reintroduce §3's bug.

Watch the polarity trap while you are in there: `SoundGraphSound::m_Priority` is
miniaudio-flavoured (**0 = highest**) while `VoiceParams::Priority` is the other way round
(**1 = highest**). `BuildVoiceParams` inverts. Getting that backwards makes the most
important sounds the first ones stolen, and every test that only counts voices still
passes.

## 8. Adding a field to `AudioSourceConfig` is four edits, and one of them is silent

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

## 9. Re-score on every input that moves

The score is `Priority × gain × distanceAttenuation`, so **every** mutator feeding those
inputs has to push fresh params at the manager — `SetVolume`, `SetPitch`, `SetLooping`,
`SetPosition`, `SetMinDistance` / `SetMaxDistance`, `SetSpatialization`, `SetPriority`, and
`SetConfig`. Miss `SetPosition` and distance-based stealing silently ranks every voice on
where it was when it started; the mix still sounds plausible, and no test that never moves
an emitter will notice.

`Scene::UpdateAudio` ticks the manager **last**, after the listener sync, the per-source
position sync, and `AudioEventsManager::Update` — so the rebalance sees this frame's
spatial state and this frame's newly triggered sounds, not the previous frame's.
