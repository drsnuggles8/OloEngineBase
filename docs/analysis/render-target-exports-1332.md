# Render-target exports: measured before and after (issue #1332)

**Rule.** A scene attachment that later passes read is a graph-owned view of that attachment, never a
copy refreshed by each writer. A copy is kept only where a view cannot serve the reader, and this file
lists every such copy with its reason. Count them with `OLO_RG_COPY_LEDGER=1`, which logs every image copy and
blit a frame issues, by pass, with bytes.

The Forward and Forward+ exports `SceneDepth`, `SceneNormals` and `Velocity` were transient textures that
ScenePass filled by copying its attachments. Every later geometry pass then had to copy them again. A writer
that drew without copying left its readers the attachment as it was before that writer, and nothing
reported it. They are now attachment views of `SceneColor`. Deferred was already a view of the G-Buffer and did not
change.

## How it was measured

`RenderTargetExportEvidence.EveryCellReportsItsCopiesBytesTimeAndPeak` renders the real pipeline at
1920x1080 (GL, Debug) with GTAO, TAA and motion blur on, with and without late geometry: foliage, a
GPU-driven instanced field (>1024 instances, HZB occlusion), a decal and water. For every cell it writes
`OloEditor/assets/tests/exports/RenderTargetExports_<cell>.json`, which records:

- every copy and blit, named by pass, source and destination;
- the transient pool's capacity, demand and alias savings;
- the frame's peak resident bytes;
- the median GPU time of every pass over 12 frames.

The *before* figures come from commit `59c024fc8` (the ledger, before the migration); the *after*
figures come from the PR head.

## Before: the export copy graph

Forward, late geometry, one frame (Forward+ is identical):

| pass | copies (destination, bytes at 1080p) |
|---|---|
| ScenePrepassPass | SceneDepth 8.3 MB, SceneNormals 8.3 MB, ForwardAODepth 8.3 MB |
| GPUDrivenOcclusionPrepassPass | SceneDepth, SceneNormals, ForwardAODepth (8.3 MB each) |
| FoliagePrepassPass | SceneDepth, SceneNormals, ForwardAODepth (8.3 MB each) |
| ScenePass | SceneDepth 8.3 MB, SceneNormals 8.3 MB, Velocity 16.6 MB |
| GPUDrivenOcclusionPass | SceneNormals 8.3 MB (never Velocity) |
| FoliagePass | Velocity 16.6 MB, SceneDepth 8.3 MB |

That is 15 export copies, 141 MB per frame. GroomPass, DecalPass, WaterPass, the fluid composite and the
particles write the scene target after the last copy and refresh nothing.

Deferred issues no export copy: its exports already were views of the G-Buffer. The copy code in
VirtualGeometryPass, DeferredGPUOcclusionPass and DeferredOpaqueDecalPass compared the export with its
source, found the same object, and returned. It never copied anything.

## After

| cell | export copies (MB) | all copies + blits (MB) | pool demand MB | peak resident MB |
|---|---|---|---|---|
| Forward | 6 (58.1) -> 1 (8.3) | 9 (93.3) -> 4 (43.5) | 204.8 -> 171.7 | 538.0 -> 504.8 |
| Forward, late geometry | 15 (141.0) -> 5 (41.5) | 19 (192.8) -> 9 (93.3) | 221.4 -> 196.6 | 581.8 -> 556.9 |
| Forward+, late geometry | 15 (141.0) -> 5 (41.5) | 19 (192.8) -> 9 (93.3) | 221.4 -> 196.6 | 581.8 -> 556.9 |
| Forward, upscale (Performance), late | 15 (35.3) -> 5 (10.4) | 19 (73.1) -> 9 (48.2) | 157.0 -> 150.7 | 597.8 -> 591.5 |
| Deferred | 0 -> 0 | 5 (51.8) -> 5 (51.8) | 163.4 -> 163.4 | 687.3 -> 687.3 |
| Deferred, late geometry | 0 -> 1 (8.3) | 6 (68.4) -> 7 (76.7) | 180.0 -> 188.3 | 706.2 -> 714.5 |
| Deferred MSAA 4x | 0 -> 0 | 13 (151.4) -> 12 (143.1) | 163.4 -> 163.4 | 1068.9 -> 1077.2 |
| Deferred MSAA 4x, late geometry | 0 -> 1 (8.3) | 28 (350.4) -> 28 (350.4) | 180.0 -> 188.3 | 1071.1 -> 1079.4 |

The one Deferred copy added is the water's view-normals snapshot. It fixes a feedback loop (see the table of
retained copies below). The MSAA cells lose ScenePass's second depth resolve (`GBuffer::ResolveColorOnly`). The
peak in the MSAA cell without late geometry is 8.3 MB higher because the pool still holds the previous cell's
snapshot; its demand is unchanged.

**GPU time.** Within one run, a pass's total minus its own draw sub-pass is the time the pass spends
outside its draws, which is where its copies were. With Forward and late geometry, that time dropped from
0.142 to 0.039 ms in ScenePrepassPass and from 0.108 to 0.003 ms in ScenePass, so about 0.21 ms per frame
at 1080p in these two passes. The prepass shares and FoliagePass lose their copies too. Totals across runs
are not comparable to better than about 20%: the before run's draw sub-passes (unchanged code) were 10–25%
slower, and the Deferred MSAA cells vary between 1.0 and 1.9 ms between identical runs.

## Retained copies and why (AC4)

| copy | made by | why it is not a view |
|---|---|---|
| ForwardAODepth (1 per prepass writer, Forward with screen-space AO) | ScenePrepassPass, the GPU-driven and foliage prepass shares | Every forward shader samples it for the AO upsample while it depth-tests against the live attachment. A view would be a feedback loop. |
| SceneDepthSnapshot (Forward, when decals or water draw) | SceneDepthSnapshotPass, after GroomPass | Decals and water sample depth while drawing into SceneColor. On Deferred the snapshot is SceneDepth itself, the G-Buffer's depth, which they do not draw into. |
| SceneViewNormalsSnapshot (every path, when water draws) | SceneViewNormalsSnapshotPass, right before WaterPass | Water marches view normals while `Water.glsl` writes them to the same attachment. Before #1332 it sampled that attachment live. |
| GBufferMS -> GBufferResolved blits (Deferred MSAA) | ScenePass, DeferredGPUOcclusionPass, DeferredOpaqueDecalPass | A real MSAA resolve. Each late writer that draws into the multisample G-Buffer resolves all seven attachments again, 91 MB each at 1080p. See *Not changed* below. |
| G-Buffer depth and entity ID -> SceneColor blits (Deferred) | DeferredLightingPass | The forward overlay, groom and decals depth-test in SceneColor, and picking reads its entity ID. |
| SceneColorTexture -> WaterRefraction | WaterPass | The refraction colour is read while the water draws over it. |
| Velocity -> TAASurfaceHistory, TAAColor -> TAAHistory | end-of-frame extraction | Temporal histories outlive the frame. Velocity is now a view, so the planner keeps SceneColor alive until the copy runs (commit `36056b436`). |

Diagnostic captures stay explicit: `olo_render_capture_target` and `RenderGraphPassSnapshot` clone into
scratch storage of their own (the ledger names it `<external>`, attributed to `<post-pass hook>`).

## Freshness (AC2)

`RenderTargetExportEvidence.EveryExportConsumerReadsTheLastWriteOfItsAttachment` walks a real frame. For each
pass that reads an export, either the export resolves to the attachment itself, or the copy that last
refreshed it came after every pass that wrote the attachment. On the copy model it reports 7 stale reads on
Forward and on Forward+:

- VSM marking reads `SceneDepth` from before the GPU-driven batches;
- Water, TAA, motion blur and tone mapping read `SceneDepth` from before decals and water;
- TAA and motion blur read `Velocity` from before decals and water.

It reports 0 after the migration. In pixels,
`TemporalResolveSamplesTheVelocityTheLateGeometryWrote` compares, after TAAPass, the velocity TAA sampled with
the scene target's attachment 3. On the copy model 49 063 texels at 1080p differ: the GPU-driven batches
never refreshed velocity, so TAA reprojected them with the motion of whatever lay behind them. After the
migration, 0 texels differ.

## Lifetime (AC3)

- The views follow their parent's lifetime: a read of an attachment view extends the parent framebuffer's
  transient lifetime.
- A history extracted from a view now extends the parent too. Before `36056b436` it did not, and
  `RenderGraphAttachmentViewExports.AHistoryExtractedFromAViewKeepsItsFramebufferAlive` shows two framebuffers
  sharing alias slot 0.
- `AliasingAndCaptureLeaveTheExportsAndTheFrameUnchanged` renders Forward and Deferred three ways: as is,
  with transient aliasing off (`OLO_RG_DISABLE_ALIASING`), and with a capture snapshot armed on the three
  exports. Scene depth is bit-identical in all three and the composite differs by RMSE 0.

## A consumer that changed behaviour: TAA on the stochastic groom coat

Forward TAA now reads the groom's own velocity. It used to read the cleared zero behind the coat, because the
copy predated GroomPass. Every surface's velocity carries the projection jitter delta: 0.00073 UV at a static
camera, the engine's convention (`RenderPipeline.cpp`). So TAA now reprojects the coat as it does every
surface, and the stochastic coat's shimmer reduction on Forward falls from 11x to 3x. Deferred keeps 11x
only because its resolve reads G-Buffer velocity, which the groom never writes. That also means a moving coat
on Deferred has no velocity in TAA at all.

The groom temporal tests were re-baselined to the honest velocity, with the measured numbers in their
comments. The velocity convention and Deferred groom motion are #1552.

## Not changed, and why

- **Deferred MSAA resolves.** With late geometry, the G-Buffer is resolved three times a frame (ScenePass,
  DeferredGPUOcclusionPass, DeferredOpaqueDecalPass: 21 blits, about 270 MB at 1080p). Resolving once after
  the last multisample writer is the deferred family's migration, #1553. This PR removes only ScenePass's
  second depth blit on the per-sample path (`GBuffer::ResolveColorOnly`).
- **The validator's blind spot.** A pass that samples an attachment view while writing a renamed version of
  the same framebuffer is never reported as feedback, which is how water's loop went unseen: #1554.
- **Deferred depth blit into SceneColor.** It is needed for overlay depth testing, as listed above.
