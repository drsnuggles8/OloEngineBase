# Command packets: prepare, freeze, replay, retire

**Rule: write a command packet, a command bucket or a `FrameDataBuffer` range only before its first
replay. A pass that needs a different version of a frozen packet copies it with
`CommandPacket::Clone` and edits the copy.** Issue #1335 made this checkable; this page says what is
checked, what each check catches, and what it costs.

## The four states

| State | Who is in it | What may happen |
|---|---|---|
| Prepare | a packet from `Submit` / `CreateDrawCall` / `AllocatePacketWithCommand`; a bucket after `Clear` / `Reset`; the frame's payloads after `FrameDataBuffer::Reset` | everything: fill the command, set metadata, remap bone offsets, sort, batch, write payloads |
| Freeze | the first replay of a bucket (`Execute`, `ExecuteParallel`, `ExecuteWithGPUTiming`) or of a loose span (`RecordPackets`), or an explicit `CommandBucket::Freeze()` | the packets' bytes and the bucket's order become final; `FrameDataBuffer::PublishForReplay` makes every range allocated so far read-only |
| Replay | any number of replays, serial or on workers | reads only. Statistics live in the caller's `Statistics`, never in a packet |
| Retire | `CommandBucket::Clear` / `Reset`, `FrameDataBuffer::Reset` | back to Prepare. A packet is never un-frozen: its memory goes back with the allocator |

Appending is always legal. A later pass may allocate and write NEW `FrameDataBuffer` ranges after an
earlier pass replayed (`SceneRenderPass` batches after `ShadowPass` has replayed), because the
publication is a watermark below which nothing may change.

## What is enforced

Every check reports through `CommandLifecycle::ReportViolation`: a counter per kind, an error line
in the log on the 1st, 2nd, 4th, 8th… occurrence, and an assert in Debug.

| Violation | Raised by | Refused? |
|---|---|---|
| `PacketMutatedAfterFreeze` | a mutating `CommandPacket` accessor (`GetCommandData<T>()`, `GetRawCommandData()`, the setters) on a frozen packet | no: the accessor returns a pointer and cannot refuse. Read through the const view instead |
| `BucketMutatedAfterFreeze` | `Submit`, `AddCommand`, `SubmitPacket(Parallel)`, `Sort` (when it would reorder), `Batch`, `RemapBoneOffsets`, the parallel-submission calls on a frozen bucket | yes, the operation does nothing |
| `PayloadWrittenAfterPublish` | a `FrameDataBuffer::Write*` below the watermark | yes, the write does nothing |
| `ReplayOfUnfrozenBucket` | `ReplayRange` (const, cannot freeze) on a bucket nobody froze | yes, nothing replays |
| `PacketChangedAfterFreeze` | validation: a frozen packet's digest differs before a replay | yes, the whole replay is skipped before any worker starts |
| `PacketChangedDuringReplay` | validation: a frozen packet's digest differs after a replay | no, it already happened; reported when the last worker finishes |

`CommandBucket::GetPackets()` is a `std::span<const CommandPacket* const>`, and `FrameDataBuffer` has
no mutable pointer accessors: reading a frozen frame cannot reach a mutating overload by accident.

**Validation** records a 64-bit digest of each packet (type, size, dispatch function, metadata, and
the inline command bytes) when it freezes, and checks it at every replay. It is the only check that
sees a write through a pointer taken during preparation. It is on in Debug and in the test binary,
off in Release, and `OLO_COMMAND_LIFECYCLE_VALIDATION=1` turns it on anywhere, including in a running
editor through `olo_debug_levers_set`.

## If you replay packets with your own loop

`RecordPackets` and the `Execute` family bracket every replay themselves. A pass that walks packets
with its own loop (today only `DecalRenderPass::ExecuteOnGBuffer`) must freeze first and bracket the
loop:

```cpp
m_CommandBucket.Freeze();
if (CommandBucket::BeginReplay(packets))   // publish, freeze, validate — on the forking thread
{
    RenderCommand::RecordParallel(...);    // workers read only
    CommandBucket::EndReplay(packets);     // validation: report writes that raced the replay
}                                          // a refused replay ran nothing, so it has no end
```

## What it costs

Measured by `CommandLifecycleCost.PerPhaseHotPathCost` (10 000 draws, 100 batchable groups, Release,
clang-cl, Intel Core i7-14700KF). Before is `c5cf5c6a9`; after is this change.

Four interleaved before/after runs of 25 repetitions each. The box swings medians by about 2x
between runs, so the fastest repetition of each run is the comparable figure:

| Phase, 10 000 draws | before (min per run) | after (min per run) |
|---|---|---|
| submit (allocation + packet construction) | 1.19–1.51 ms | 1.23–1.38 ms |
| sort (radix, 64-bit keys) | 0.43–0.49 ms | 0.43–0.48 ms |
| batch (group, collapse, re-sort) | 0.73–1.05 ms | 0.80–0.95 ms |
| replay walk, validation off (Release) | 0.040–0.052 ms | 0.058–0.072 ms |
| replay walk, validation on | — | 0.59–0.65 ms |

Submission, sort and batch are unchanged within noise. The Release replay pays about 2 ns per packet
for the freeze walk; validation pays about 60 ns per packet for the digests, which is why it is off
in Release. The packet header grew from 56 to 64 bytes (the digest), so a 10 000-draw frame
allocates 3.20 MB instead of 3.12 MB.

## The failures it prevents

- `DecalRenderPass` installed the Decal_OIT program override by writing every queued decal packet —
  after `ExecuteOnGBuffer` had already replayed that bucket in the Deferred path. The write was
  sequential, so it was harmless on that day; nothing stopped the same pattern on a bucket whose
  replay runs on workers. It now edits clones.
- A replay's dispatcher took `FrameDataBuffer&` and reached its mutable pointer accessors while
  reading. The accessors are gone.
- A second submission into a bucket after its replay would have been dropped silently by the next
  frame's reset. It is now refused and reported at the call.
- **Retiring one bucket must not retire another's packets.** Every render-stream pass shares
  `FrameResourceManager`'s per-frame allocator, and `CommandBufferRenderPass::ResetCommandBucket`
  used to reset that allocator. A stream pass that retired its bucket early in the graph freed every
  other stream's packets before they replayed. The Forward decal frame survived it only because
  nothing allocated in between; the first allocation that did (the OIT clone) landed on the decal
  packet and its type read back as `Invalid`. A pass now resets only an allocator it owns. If you
  give a pass a shared allocator, its retire is `Clear()` plus `ResetStatistics()`, never `Reset`.
