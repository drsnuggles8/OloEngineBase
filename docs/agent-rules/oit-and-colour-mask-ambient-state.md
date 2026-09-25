# OIT and colour-mask ambient state

Rule: a pass that draws into the weighted-blended OIT targets must state its whole target state
(write masks and per-attachment blend) after anything that can change it. A pass that replays
command packets must widen the colour write mask when it finishes. A per-pixel identity stored in
a render target is decoded from the texel, never from a filtered fetch.

Each of these produced a frame that looked plausible and passed every headless test: #1417, #1422.

## A global `SetBlendFunc` inside an OIT pass erases both OIT attachments' blend

Weighted-blended OIT needs two blend functions at once: `One, One` on the accumulation target and
`Zero, OneMinusSrcColor` on the revealage target. They are per-attachment state
(`ApplyWeightedBlendedOITBlend`, `Renderer/OITBlendState.h`). On GL a global `glBlendFunc` resets
every draw buffer's function, so anything inside the pass that sets a global blend removes both.
Re-state the pair after any such call.

It happened twice in #1417. `CommandDispatch::DrawDecal` applied the transparent decal packet's own
render state (`SrcAlpha, OneMinusSrcAlpha`), and the decal visibility diagnostic applied it a
second time after its own draw. `Scene`'s per-emitter `SetParticleBlendMode` did the same for
particles. The draws were issued and their fragments survived, but the accumulation overflowed
RGBA16F to `inf` and the revealage stayed at its clear value of 1. `OITResolve` discards a pixel
whose revealage is 1, so the composite was byte-identical to a frame without the draw. Reading the
two OIT targets back right after the draw found the fault; the render graph's pass order was
correct.

## A pass that replays packets must widen the colour write mask when it finishes

A packet narrows per-draw-buffer write masks through its render state (`colorAttachmentWriteMask`,
`colorAttachmentChannelMask`), and only the next packet's global `SetColorMask` widens them again.
So the last packet's narrowing outlives the pass unless the pass ends with
`SetColorMask(true, true, true, true)` and `CommandDispatch::InvalidateRenderStateCache()` (the
widening happened behind the dispatcher's state cache). `ScenePass`, `ForwardOverlayRenderPass`
and both `DecalRenderPass` replays now do. In the editor, `ScenePass` ended on a skeleton/joint draw
that keeps to RT0, and draw buffer 1 stayed masked until `AOApplyPass`. That silently dropped three
things: `DeferredLightingPass`'s skin-diffusion hand-off (so Deferred diffusion did nothing live),
`OITPreparePass`'s revealage clear (so OIT turned every Forward frame black) and every OIT draw's
revealage write (#1417, #1422). Headless fixtures draw no editor gizmos, so no test saw it. A GL
colour clear also honours the mask; the backend lifts masks around both `glClear` and
`ClearFramebufferColorAttachment`. To find a leak like this, log `glGetBooleani_v(GL_COLOR_WRITEMASK, i)`
before and after each pass in `RenderGraphPlanExecutor`.

## Decode a per-pixel identity lane from the texel, never from a filtered fetch

Skin diffusion stores a profile slot in the aux target's alpha as `(slot + 1) / 8`. A bilinear
fetch blends the lane of four texels, and the decoded result depends on the slot value: at a skin
edge the blend decodes as slot `s` only while the skin weight is at least `(s + 0.5) / (s + 1)`.
That is 50% for slot 0 and 93% for slot 6, so the same tap was kept in one slot and rejected in
another (#1422). Identical profiles in two slots rendered terminators up to 3/255 apart, and
`SkinDigitalHumanToneGrid`'s scattering ladder had silently depended on Fair being in slot 0.
`SkinDiffusion.glsl` now gathers the four lanes and rejects a tap if any texel the bilinear fetch
weights names another slot. Do not shortcut it with "the blended lane equals this slot's code": the
codes are linear, so a 50/50 blend of slots s-1 and s+1 lands exactly on slot s. Treat any value
used as an ID the same way.
