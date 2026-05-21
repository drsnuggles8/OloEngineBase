# SoundGraph refactor: adopt MetaSounds-style block execution

Working plan to bring OloEngine's SoundGraph runtime in line with the
architecture Unreal's MetaSounds uses. Replaces the current per-sample graph
traversal — ported from Hazel, originally inspired by SOUL/Cmajor — that does
not survive a Debug build.

## Why

The current runtime calls `SoundGraph::Process()` once per audio sample
(48 000 times/sec). Each call walks every node, performs
`std::unordered_map::find` on the endpoint map, dispatches virtual `Process()`
on each node, and reads through `choc::value::ValueView`. In Release this
inlines acceptably. In Debug with `_ITERATOR_DEBUG_LEVEL=2` it does not.

Measured on the editor preview path (HelloDing.olosoundgraph, ding.wav
44.1 kHz stereo, 3.71 s):

- Expected: 100 `ProcessSamples` callbacks of 480 frames = 1.0 s of audio in
  ~1000 ms wall time.
- Observed (Debug, MSVC): **5274 ms wall time, effective output rate
  9101 Hz** — 5.27× slower than real time. The ding stretched audibly.

The design was inherited from SOUL, which AOT-compiles graphs to native code
where the per-sample loop collapses into straight-line arithmetic. We
implemented the DSL surface but not the compiler, so every "elegant" abstraction
runs through `std::function`, `unordered_map`, and virtual dispatch at audio
rate. The fix is to switch the runtime to the block-processing model every
shipping game engine uses (FMOD, Wwise, Unity DSPGraph, Unreal MetaSounds,
JUCE).

## Architecture decisions copied from MetaSounds

1. **Block-based `Execute`.** Each operator processes a whole block of frames
   per call (256–512 typical). Never per-sample.
2. **Typed connections.** Connections distinguish what timing each value
   needs: `AudioBufferRef` (block-rate audio), `TriggerRef` (sample-accurate
   events carrying a frame offset within the block), `FloatRef` (control-rate
   parameters). The editor enforces type compatibility at wire time.
3. **Compiled execution plan.** On graph load, topologically sort the nodes
   and lower the graph to a flat array of operator handles. Audio thread walks
   that array. No hash lookups, no virtual dispatch, no `std::function`
   indirection in the hot path. This is the "compiler" part — flat-opcode
   interpreter, not LLVM.
4. **Sample-accurate triggers via frame offsets.** Triggers carry
   `i32 SampleOffset` within the block. Nodes that consume triggers (e.g.
   WavePlayer's Play / Stop) split their block processing at offsets. Get
   sample-accurate firing without sample-rate graph traversal.

## Out of scope

- LLVM-based codegen. The flat-opcode interpreter in Phase 3 captures the
  per-frame win without adding a 50 MB dependency.
- Source-generating C++ and shelling out to a compiler. Kills the edit-play
  iteration loop the editor exists to provide.
- Rewriting the SoundGraph editor UI. Wiring semantics change but the editor
  layer stays the same; only the runtime is refactored.
- Porting MetaSounds verbatim. The full codebase is ~50k LOC of editor,
  serialization, and template machinery we do not need.

## Phase 1 — block-based execution

**Goal:** the ding plays correctly in Debug. Foundation for later phases.

### Signature change

```cpp
// Before
virtual void Process();

// After
virtual void Process(u32 numFrames);
```

Audio outputs become `f32*` buffers sized to `numFrames`, not single `f32`
fields. Event outputs stay as-is for now (Phase 4 introduces sample-offset
triggers; until then events fire at block boundary).

### Files affected

- `OloEngine/src/OloEngine/Audio/SoundGraph/NodeProcessor.h` — base virtual
  signature, output stream registration.
- `OloEngine/src/OloEngine/Audio/SoundGraph/SoundGraph.h` — `Process` becomes
  per-block; per-frame sync loop deleted (no longer needed).
- `OloEngine/src/OloEngine/Audio/SoundGraph/SoundGraphSource.cpp` —
  `ProcessSamples` inner per-frame loop replaced with a single
  `m_Graph->Process(frameCount)` call plus a bulk copy from graph output
  buffers into `busOut`.
- `OloEngine/src/OloEngine/Audio/SoundGraph/Nodes/WavePlayer.h` — `Process`
  body wrapped in a per-block loop; output written into provided buffer.
- `OloEngine/src/OloEngine/Audio/SoundGraph/Nodes/{Math,Generator,Music,
  Envelope,Trigger,Array}Nodes.h` — same wrap-and-iterate pattern for the
  nodes the editor exercises.

### Non-goals for this phase

- Don't redo connections — leave `choc::value::ValueView` wiring in place.
  Phase 2 replaces it.
- Don't add a flat operator list yet — keep the existing `m_Nodes` vector
  walk. Phase 3 flattens it.
- Don't change the editor or serialization.

### Definition of done

- `ProcessSamples` does one `m_Graph->Process(frameCount)` per block, no
  inner per-frame loop.
- `[SGSDiag] After 100 ProcessSamples calls` reports `effectiveRateHz` within
  10% of the configured rate (48000) on Debug.
- HelloDing.olosoundgraph plays the ding in ~0.5 s of wall time, not 5 s.
- Existing SoundGraph instantiation/serializer tests still pass.

## Phase 2 — typed connections

Replace `choc::value::ValueView`-based wiring with explicit reference types.
The current double-bookkeeping (`InputStreams` ValueView vs `m_ParameterStorage`
`std::any`) goes away.

- `AudioBufferRef` — `Span<f32>` into a producer's output buffer.
- `FloatRef`, `IntRef`, `BoolRef` — `const T*` into a producer's scalar
  output.
- `TriggerRef` — placeholder for Phase 4.

`AddConnection` becomes a typed function template. The editor's connection
validation moves to compile-time concept checks. `unordered_map<Identifier,
ValueView>` becomes `unordered_map<Identifier, AnyRef>` (or split per-type
maps if profiling says so).

Mostly a wiring/cleanup phase. No new runtime behavior. Removes the parameter
overlay hack in `NodeProcessor::ApplyAssetDefaultToParameter`.

## Phase 3 — compiled execution plan

The "compiler" part, without LLVM. Done once on graph load, used every
callback.

### Compile step (off audio thread)

1. Topologically sort `m_Nodes` from inputs to outputs.
2. Lower to `std::vector<IOperator*>` (or function pointers + state pointers
   for cache-friendliness).
3. Snapshot all `AudioBufferRef` / `FloatRef` / `TriggerRef` pointers into a
   flat layout so each operator reads/writes by index, not by hash lookup.
4. Pre-size all per-node output buffers (`numFrames` floats each), stored in
   a contiguous pool.

### Run step (audio thread)

```cpp
void SoundGraph::Process(u32 numFrames) {
    for (auto* op : m_CompiledOps) {
        op->Execute(numFrames);
    }
    // outputs are at known offsets in the buffer pool; copy to graph outputs
}
```

No `unordered_map`, no virtual dispatch through a base pointer if we store
function pointers directly. This is the part that gives Debug the breathing
room to keep real-time without optimization.

Delete `m_NodeLookup`, `m_EndpointOutputStreams.InputStreams` lookups in the
hot path, and the `m_OutputChannelViews` cache (Phase 1 stopgap).

## Phase 4 — sample-accurate triggers

Only worth doing if and when sample accuracy actually matters. Editor preview
does not need it. Game-runtime triggers (footsteps synced to animation events,
weapon SFX synced to frame-accurate gameplay) eventually will.

```cpp
struct Trigger {
    i32 SampleOffset;  // within the current block; -1 means "not fired this block"
};
```

Nodes that consume triggers (WavePlayer Play/Stop, envelope retrigger, etc.)
inspect `SampleOffset` and split their `Execute` at that frame index.

## Decision log

- **2026-05-20.** Chose block-based runtime over LLVM-backed JIT. Block
  processing is the industry standard (FMOD, Wwise, Unity, Unreal, JUCE all
  use it); JIT pulls in a 50 MB dependency for a marginal win once Phase 3
  flattens dispatch. Reversible if profiling later proves otherwise.
- **2026-05-20.** Chose phased rollout (1 → 4) over big-bang rewrite. Each
  phase ships an observable improvement; we can stop after Phase 1 if the
  perf bug is solved and the architecture is acceptable.
