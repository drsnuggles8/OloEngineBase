# A pass opens the colour mask of every attachment it writes

**Rule.** The colour write mask is shared state, and every pass owns both ends of it:

- **Before it draws**, a pass sets the mask of every attachment it writes, with the global
  `RenderCommand::SetColorMask(true, true, true, true)`. The global setter also resets every
  per-attachment mask; see [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md).
  The mask the pass finds is whatever the previous pass's last draw left, which is somebody else's
  state.
- **Before it returns**, a pass that narrows the mask (a prepass, a masked overlay) or replays
  command packets widens it again with the same call, then calls
  `CommandDispatch::InvalidateRenderStateCache()`. The widening happened behind the dispatcher's
  state cache. Without the invalidate, the next packet that shares the narrowing packet's
  frame-deduplicated render-state index skips re-applying its own narrowing.

Do both. Either half alone leaves a gap: a pass that restores cannot cover a later pass that forgot
to open, and a pass that opens cannot cover an earlier pass that forgot to restore.

Selecting the draw buffers (`SetFramebufferDrawAttachments`) is not enough. Draw buffers say where a
fragment output goes; the mask says whether it is written. A pass can route its second output to
the right texture and still write nothing there. A GL colour clear honours the mask too; the
backend lifts masks around both `glClear` and `ClearFramebufferColorAttachment`.

## The same fault, found twice in one day

On 2026-09-25 two branches running in parallel hit this leak independently and fixed it from
opposite ends. Both fixes are on master and both stay.

- **#1469 (#1417, #1422) fixed the producer.** A packet narrows its draw buffers through its render
  state (`colorAttachmentWriteMask`, `colorAttachmentChannelMask`), and only the next packet's
  global `SetColorMask` widens them again. In the editor, ScenePass ended on a skeleton or joint
  draw that keeps to RT0, so draw buffer 1 stayed masked until `AOApplyPass`. That silently dropped
  `DeferredLightingPass`'s skin-diffusion hand-off (Deferred diffusion did nothing live),
  `OITPreparePass`'s revealage clear (OIT turned every Forward frame black) and every OIT draw's
  revealage write. `ScenePass`, `ForwardOverlayRenderPass` and both `DecalRenderPass` replays now
  end with the widen and the invalidate.
- **#1475 (#1451, #1452) fixed the consumer.** The same leak had left `DeferredLightingPass`
  writing scene colour but not the hand-off in attachment 4, so the new snow blur changed no
  Deferred pixel either. `DeferredLightingPass` now opens every attachment before its MRT draw.

Headless fixtures draw no editor gizmos, so no test saw either instance: only the live editor ended
the G-Buffer bucket on a masked draw.

## Other instances

- **The forward depth prepass left the global mask off** (#1452). A prepass draws with every
  attachment masked. Once the forward prepass became its own node ahead of screen-space AO, SSAO's
  raster draw inherited the all-false mask, and the AO buffer stayed at its clear value (flat
  white, "no occlusion"). `RunDepthPrepass` now restores the mask and invalidates the cache.
- **Debug lines were masked for the wrong layout** (#1472). `Renderer3D::DrawLine` keeps a line out
  of attachments 1 and 2 because on the forward scene framebuffer those are entity ID and view
  normal. On Deferred the same draw went into the G-Buffer, where the mask kept it out of the
  emissive lane. With depth test off it wrote no depth either, so over sky the lighting pass shaded
  it as background, and every debug line was invisible on Deferred. Such draws now go to
  `ForwardOverlayPass` there, where the mask means what it was written for.

## How to find one: query the mask, not the output

A texture at its clear value looks like "the shader never wrote it", and that sends you to the
shader. Two cheap measurements localise it instead:

- **After every graph node**, log `glGetBooleani_v(GL_COLOR_WRITEMASK, i, ...)` for the attachment in
  question (temporary code in `RenderGraphPlanExecutor.cpp`, next to `NodePointer->Execute`). The
  first node that leaves it off is the culprit's pass. For both instances above that was ScenePass.
- **Inside that pass**, bracket its stages the same way. The colour bucket was the stage; a log line
  in `ApplyPODRenderState`'s per-attachment loop then named the render state (write mask `0x1`).

Forcing the mask on at the consumer first, as a one-line hypothesis check, is worth doing before
either: it turned a vague "the hand-off is lost" into a yes/no in one build.

## Checklist for a new pass

- Does it write more than attachment 0? Set the mask for all of them with the global setter.
- Does it narrow the mask, or replay command packets? Restore it before returning, and call
  `CommandDispatch::InvalidateRenderStateCache()` so the next packet re-applies its state.
- Does a draw's render state carry a per-attachment mask? That mask is written against one
  framebuffer layout. Check which framebuffer the draw lands in on every render path.
- Does it draw into the WB-OIT targets? It also re-states the per-attachment blend; see
  [oit-blend-state-and-identity-lanes.md](oit-blend-state-and-identity-lanes.md).
