# A pass opens the colour mask of every attachment it writes

**Rule.** A pass that draws into an attachment sets that attachment's colour write mask itself,
immediately before its draw, with the global `RenderCommand::SetColorMask(true, true, true, true)`
(the global setter also resets every per-attachment mask — see
[gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md)). The mask it
finds is whatever the last draw of the previous pass left, and that is somebody else's state. The
same holds in reverse: a pass that narrows the mask restores it before it returns.

Selecting the draw buffers (`SetFramebufferDrawAttachments`) is not enough. Draw buffers say where a
fragment output goes; the mask says whether it is written. A pass can route its second output to
the right texture and still write nothing there.

The same rule for passes that draw into the WB-OIT targets or replay command packets is in
[oit-and-colour-mask-ambient-state.md](oit-and-colour-mask-ambient-state.md).

## Three instances, one PR (#1451 / #1452)

1. **DeferredLightingPass wrote no diffusion hand-off.** It draws scene colour and the hand-off in
   attachment 4 in one fullscreen pass. The G-Buffer command bucket before it can end on a debug
   line or skeleton joint, whose render state narrows the mask to attachment 0
   (`colorAttachmentWriteMask = 0x01`). Those per-attachment masks outlived ScenePass, so the
   hand-off write reached nothing: skin had no deferred diffusion and the new snow blur changed no
   Deferred pixel, whenever such a draw came last. Nothing errored; the texture just kept its clear
   value.
2. **The forward depth prepass left the global mask off.** A prepass draws with every attachment
   masked. The new forward prepass ran before the screen-space AO passes, and SSAO's raster draw
   inherited the all-false mask: the AO buffer stayed at its clear value (flat white, "no
   occlusion"). `RunDepthPrepass` now restores the mask and invalidates the render-state cache.
3. **Debug lines were masked for the wrong layout.** `Renderer3D::DrawLine` keeps a line out of
   attachments 1 and 2 because on the forward scene framebuffer those are entity ID and view
   normal. On Deferred the same draw went into the G-Buffer, where the mask kept it out of the
   emissive lane, and with depth test off it wrote no depth, so over sky the lighting pass shaded
   it as background. Every debug line was invisible on Deferred. Such draws now go to
   `ForwardOverlayPass` there, where the mask means what it was written for.

## How it was found: query the mask, not the output

A texture at its clear value looks like "the shader never wrote it", and that sends you to the
shader. Two cheap measurements localise it instead:

- **After every graph node**, log `glGetBooleani_v(GL_COLOR_WRITEMASK, i, ...)` for the attachment in
  question (temporary code in `RenderGraphPlanExecutor.cpp`, next to `NodePointer->Execute`). The
  first node that leaves it off is the culprit's pass. Here that was ScenePass.
- **Inside that pass**, bracket its stages the same way. The colour bucket was the stage; a log line
  in `ApplyPODRenderState`'s per-attachment loop then named the render state (write mask `0x1`).

Forcing the mask on at the consumer first, as a one-line hypothesis check, is worth doing before
either: it turned a vague "the hand-off is lost" into a yes/no in one build.

## Checklist for a new pass

- Does it write more than attachment 0? Set the mask for all of them with the global setter.
- Does it narrow the mask (a prepass, a masked overlay)? Restore it, and call
  `CommandDispatch::InvalidateRenderStateCache()` so the next packet re-applies its state.
- Does a draw's render state carry a per-attachment mask? That mask is written against one
  framebuffer layout. Check which framebuffer the draw lands in on every render path.
