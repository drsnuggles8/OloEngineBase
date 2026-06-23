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

> **Status (2026-05-25):** Phase 1A — *signature change and structural rework* —
> has landed. `SoundGraphSource::ProcessSamples` now makes a single
> `m_Graph->Process(frameCount)` call per block and bulk-copies from per-channel
> output buffers into the miniaudio bus. Every `NodeProcessor` override carries
> the new `Process(u32 numFrames)` signature; stateful nodes (oscillators,
> envelopes, WavePlayer, time-counting triggers) wrap their per-sample body in
> a for-loop and hoist input reads / event-flag checks to block rate. The
> existing SoundGraph instantiation + serializer tests pass under the new API.
>
> What did **not** land: the perf metric below (effective rate ≈ 48 kHz in
> Debug). Because Phase 1's non-goal is to "leave `choc::value::ValueView`
> wiring in place", the per-node `Process(...)` calls still have to happen
> once per sample inside `SoundGraph::Process` — calling each node once per
> block would deliver only the *last* sample of the block to the next ValueView
> consumer down the chain. The amortisation work in WavePlayer (block-rate
> async-load / play-flag checks) is in place but unused until SoundGraph can
> hand it the whole block. That switch lands as part of **Phase 2** when typed
> `AudioBufferRef` connections replace the scalar `ValueView` wiring.
>
> Concretely, after Phase 1A:
>
> - ✓ `ProcessSamples` does one `m_Graph->Process(frameCount)` call.
> - ✓ Existing SoundGraph instantiation + serializer tests pass.
> - ✓ HelloDing Debug perf — landed with Phase 2's buffer-rate wiring (see the
>   Phase 2 status block below).

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

> **Status (2026-06-11): landed**, including the block-rate flip Phase 1
> deferred. What shipped (see `StreamRefs.h` for the type definitions):
>
> - `AudioBuffer` (fixed-capacity per-output block buffer, stable `Data()`),
>   `AudioBufferRef` (stride-0 scalar broadcast / stride-1 buffer read, with
>   an inline unconnected-default cell), `ValueRef<T>` (`FloatRef` / `IntRef` /
>   `Int64Ref` / `BoolRef`, never null — points at its own default until
>   wired), and the `TriggerRef` Phase 4 stub.
> - `NodeProcessor` carries `InputRefs` / `OutputSources` typed endpoint
>   registries (the plan's `unordered_map<Identifier, AnyRef>`; profiling did
>   not justify per-type splits). `ConnectStreams` validates type compatibility
>   at wire time and patches the consumer's ref; the compile-time-typed
>   `SoundGraph::AddConnection` overloads cover code-constructed graphs, gated
>   by the `StreamValue` concept.
> - Deleted: `StreamWriter`, `InputStreams`/`OutputStreams` ValueView maps,
>   `ParameterWrapper` + `m_ParameterStorage` (`std::any`),
>   `ApplyAssetDefaultToParameter`, `EndpointUtilities::InitializeInputs`, and
>   the `m_OutputChannelViews` Phase 1 stopgap. Asset default plugs now write
>   the ref's default cell directly (`SetInputDefault`).
> - Rate model: every f32 node output is audio-rate (`AudioBuffer`); int/bool
>   outputs are control-rate scalars. Float math nodes process per-frame, int
>   math nodes once per block. Graph float parameters keep per-sample ramp
>   fidelity via a per-cell ramp buffer the graph maintains at block rate.
> - **`SoundGraph::Process(numFrames)` now calls each node once per block**,
>   in producer-before-consumer order (Kahn over the recorded connection
>   edges — a small pull-forward from Phase 3, because once-per-block calls
>   would otherwise turn a mis-ordered hop's 1-sample latency into a full
>   block). Requests larger than `kMaxAudioBlockFrames` (4096) are chunked.
> - Bonus fix: node→node *value* connections actually work now. The old
>   ValueView re-aliasing only fed the graph-output path; interior value wires
>   silently delivered nothing because nodes read their `ParameterWrapper`
>   copies. `SoundGraphTypedConnectionTest.cpp` pins the working behavior.

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

> **Status (2026-06-15): landed.** The compiled execution plan is in place:
>
> - **Flat operator list.** `SoundGraph::CompileExecutionPlan()` lowers the
>   topological `m_ProcessOrder` to `std::vector<CompiledOp>` — `{ProcessFn,
>   NodeProcessor*}` pairs the audio thread walks directly in `ProcessChunk`,
>   no vtable load. `Factory::MakeNode<T>` patches each node's `m_ProcessFn` to
>   `ProcessThunk<T>` (a `T::Process` qualified, statically-bound call);
>   directly-constructed nodes (tests, the graph container) keep a
>   `VtableProcessThunk` fallback so any node remains correct, just not
>   devirtualized.
> - **Contiguous output-buffer pool.** `SoundGraph::AllocateNodeOutputPool()`
>   relocates every node's audio-output `AudioBuffer` into one contiguous
>   `m_NodeOutputPool` allocation (fixed `kMaxAudioBlockFrames` stride), replacing
>   N scattered per-output vectors. Wired into `CreateInstance` *between* node
>   creation and wiring so the producer `Data()` pointers consumers capture stay
>   valid for the graph's life (the `StreamRefs.h` pointer-stability contract).
>   `AudioBuffer` keeps a self-owned fallback by default, so graphs built outside
>   `CreateInstance` (tests) behave identically without a pool. Stride is fixed
>   (not shrunk to the runtime block size) deliberately: `SetMaxBlockSize` runs
>   *after* wiring, so a pool sized to the real block size couldn't be resized
>   without dangling every captured pointer — shrinking it is a safe future step
>   only if a graph's footprint ever matters.
> - **Already delivered by Phase 2** (verified, not re-done here): item 3
>   (snapshot refs into a flat, index-addressed layout — nodes read inputs through
>   raw-pointer `AudioBufferRef::Sample()` / `ValueRef::Get()`, no hash lookup in
>   any `Process()`), the topological order (`m_ProcessOrder`, pulled forward),
>   `m_NodeLookup` being out of the hot path (it's wiring/editor only), and the
>   deletion of `InputStreams` / `m_OutputChannelViews`.
> - **Honest perf note.** Phase 2 already removed all real-time pressure: the
>   `[SoundGraphPerf]` net processes 1 s of Debug audio in ~1.5 ms (~650x faster
>   than real time). At block rate (~100 calls/s) the per-block vtable dispatch
>   was already negligible against the per-frame DSP, so the compiled plan does
>   **not** measurably move that number (1.53 ms before → 1.56 ms after — timing
>   noise). Phase 3 here is architectural completion — an explicit compile/run
>   separation and the flat operator layout Phase 4's sample-offset trigger
>   splitting will hang off of — not a perf fix.
> - **Tests.** `SoundGraphCompiledPlan.*` (in `SoundGraphTypedConnectionTest.cpp`)
>   pin the devirtualized thunk and the contiguous pooling; the existing
>   typed-connection / instantiation / serializer suites stay green through the
>   new path.

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

> **Status (2026-06-21): landed (node-level mechanism).** Trigger-consuming nodes
> now split their per-frame `Process()` at a trigger's frame offset; the firing
> path threads that offset end to end. What shipped (see `StreamRefs.h`):
>
> - **`Trigger` value type + `TriggerRef` input ref** (`StreamRefs.h`). `Trigger`
>   carries `i32 m_SampleOffset` (`kNotFired = -1`) with `Fire()/Consume()/IsFired()/
>   Offset()/Reset()`. `Fire()` is *first-fire-wins* within a block (multiple fires
>   collapse to the earliest offset — the old Flag/dirty "N sets = one action"
>   semantics, kept sample-aware) and clamps a negative (no-frame-info) offset to
>   frame 0. `TriggerRef` mirrors `AudioBufferRef`/`ValueRef`: an inline default
>   `Trigger` plus a `Bind()` seam for future typed trigger connections (nothing
>   wires them yet — every node uses its inline default).
> - **Offset-aware events** (`NodeProcessor.h`). `OutputEvent::operator()` and
>   `InputEvent::operator()` take an `i32 sampleOffset = 0`, forwarded producer →
>   consumer; a new `AddInEvent` overload registers an offset-aware handler
>   (`void(f32, i32)`). Both default to offset 0, so every existing value-only fire
>   and route keeps its block-boundary timing — no other node changed behavior.
> - **WavePlayer Play/Stop** and the **AD / ADSR envelopes** (trigger + release)
>   replaced their block-boundary `Flag`s with `TriggerRef`s, `Consume()` the offset
>   each block, and apply `StartPlayback`/`StopPlayback` / `StartAttack` /
>   `StartRelease` at that exact frame — propagating the offset onto their own
>   `OnPlay`/`OnStop`/`OnTrigger`/`OnComplete`/… outputs so chains stay accurate.
> - **Producers** `RepeatTrigger` and `DelayedTrigger` now fire their outputs at the
>   loop's frame offset, so a metronome → envelope / WavePlayer chain retriggers
>   sample-accurately instead of snapping every tick to the block boundary.
> - **Tests.** `SoundGraphSampleAccurateTriggerTest.cpp` pins the `Trigger` type, the
>   event offset plumbing, the AD/ADSR split (a trigger at offset N shifts the whole
>   envelope by exactly N frames; offset 0 == legacy frame-0 start), an end-to-end
>   producer-fires-mid-block → wired-consumer-reacts case, and WavePlayer Play/Stop
>   consumed at their exact frame (observed via the output-event offset, since audio
>   output needs a loaded asset a unit test doesn't mount).
> - **Out of scope (follow-on).** External triggers from game code still arrive via
>   `SoundGraphSource`'s `SendInputEvent`/lock-free queue, which is drained *before*
>   `Process` and carries no per-event frame index, so an externally-scheduled
>   footstep is still block-quantised until that queue learns to carry offsets. The
>   node-level machinery and the intra-graph producer→consumer path are sample-accurate
>   today; wiring the external scheduler and typed `TriggerRef` connections is the
>   remaining increment.

## Decision log

- **2026-05-20.** Chose block-based runtime over LLVM-backed JIT. Block
  processing is the industry standard (FMOD, Wwise, Unity, Unreal, JUCE all
  use it); JIT pulls in a 50 MB dependency for a marginal win once Phase 3
  flattens dispatch. Reversible if profiling later proves otherwise.
- **2026-05-20.** Chose phased rollout (1 → 4) over big-bang rewrite. Each
  phase ships an observable improvement; we can stop after Phase 1 if the
  perf bug is solved and the architecture is acceptable.
- **2026-06-11.** Made *every* f32 stream audio-rate (block buffers) instead
  of introducing a per-pin audio/control distinction. The editor's data model
  has no pin-rate metadata, and a control-rate float wire silently delivering
  only one sample per block (e.g. envelope → amplitude) is the kind of wrong
  that no one debugs happily. Cost is 16 KB per float output (fixed 4096-frame
  buffers, allocated once for pointer stability); Phase 3's contiguous pool
  can shrink this if it ever matters. Int/bool stay control-rate scalars.
- **2026-06-11.** Pulled the topological node ordering forward from Phase 3
  (plain Kahn over recorded connection edges, stable w.r.t. authoring order,
  cycles keep authoring order with a warning). Once-per-block calls without
  ordering would give consumer-before-producer graphs a full block of latency
  per hop where the per-sample walk had one sample. Phase 3 still owns the
  flat-opcode lowering; this is just `std::vector<NodeProcessor*>` order.
- **2026-06-11.** Float graph parameters keep per-sample ramp fidelity: each
  float cell owns a block buffer the graph broadcast-fills lazily (dirty flag)
  and per-frame-fills while a `SendInputValue(interpolate=true)` ramp is
  active. The alternative (block-rate scalar updates) would have turned the
  10 ms ramps into one-step jumps — audible zipper on volume automation.
- **2026-06-11.** Moved the math/music template nodes' `RegisterEndpoints`
  out-of-line into `NodeTypes.cpp` (same pattern as the array nodes). Inline
  definitions made registration an ODR coin-flip: TUs that don't include
  `NodeDescriptions.h` (e.g. `SoundGraphFactory.cpp`) instantiated a no-op
  body and the linker was free to pick it.
