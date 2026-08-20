# A GL global setter is the indexed setter for every draw buffer — porting it as "the fallback" is a permanent leak

Postmortem of issue **#823**: on Vulkan the **deferred** editor frame was
byte-identical no matter what you changed — the same MD5 at a given camera pose
across light edits and across editor *processes*. The scene rendered as flat
grey: no directional light, no shadows, no skybox, no material colour. Forward
in the same session was perfect, and deferred on OpenGL was perfect.

## The rule GL encodes, and the half that gets ported

GL has three state pairs where a global entry point is *defined* as its indexed
form applied to **every** draw buffer:

| global | indexed | what the global does to divergence |
|---|---|---|
| `glColorMask` | `glColorMaski` | overwrites every buffer's mask |
| `glEnable`/`glDisable(GL_BLEND)` | `glEnablei`/`glDisablei` | overwrites every buffer's enable |
| `glBlendFunc`/`glBlendFuncSeparate` | `glBlendFunci` | overwrites every buffer's factors |

A backend that keeps its own per-attachment array must therefore make the global
setter **overwrite the whole array**. Model it as "per-attachment value, falling
back to the global one" and you invert the relationship: nothing ever writes the
per-attachment entry again, so **one indexed call is permanent for the rest of
the process**.

The Vulkan arm had ported exactly one of the three. `SetBlendFunc` cleared
`AttachmentBlendFuncSet` and said why in a comment; `SetColorMask` and
`SetBlendState` did not, and the lowering read
`AttachmentColorMask[i] & globalMask` and `AttachmentBlend[i] || state.Blend`.

## Why it stayed invisible for a whole phase

Two independent reasons, and both are the general lesson:

1. **The caller relies on the reset and cannot state it.**
   `CommandDispatch::ApplyRenderState` calls the global setter and then only ever
   *disables* the attachments a command's `colorAttachmentWriteMask` names — it
   never re-enables one, because the global call is supposed to have. Its comment
   says so out loud ("glColorMask above resets all buffers, then glColorMaski
   selectively disables masked-out ones"). Read the *caller's* comment when
   porting a facade entry point; the contract lives there, not in the signature.

2. **Only attachment 0 is ever looked at on the forward path.** The forward scene
   framebuffer's RT1/RT2/RT3 are entity-id, view normals and velocity — inputs to
   picking, AO and TAA, none of which anyone eyeballs — the fix turned out to
   repair forward-path screen-space AO on Vulkan as a side effect, because
   `SceneViewNormals` had been holding its clear value and GTAO was integrating
   from a constant bogus normal. So the whole editor looked fine while every
   draw in the process had been writing colour attachment 0 alone ever since the
   first narrowing command — `Renderer3D::DrawLine`, which
   every editor gizmo built from line segments goes through, sets
   `colorAttachmentWriteMask = 0x01`, and the infinite grid sets
   `0xFF & ~(1 << 2)`. The deferred path is where attachments 1..3 *are* the
   picture.

The deferred symptom then falls out of one shader line. The G-Buffer clears to
`(0.1, 0.1, 0.1, 1.0)`, and `emissiveFlags.a > 0.5` is
`ComputeDeferredLit`'s **unlit** flag. With RT2 never written, every pixel took
the unlit early-out and returned `emissive.rgb` — a uniform grey that is a pure
function of geometry and camera, immune to every light, shadow, IBL and sky
input. The geometry still visible in the viewport was the downstream AO and fog
shaping that flat grey by depth.

## The measurement that settled it

Three cheap steps, in this order, and none of them a guess:

1. **A hash A/B with a control on the same session.** Same pose, sun at
   2.6 / 40 / 0 / 2.6: deferred returned one MD5 four times, forward returned
   three different ones and reproduced its first hash exactly. That is both the
   repro *and* the noise floor (zero) in one table, and the forward column is
   what makes the deferred column mean anything.
2. **Every capture carries a liveness block — read it.** The first run of this
   A/B was taken against a window that had been minimised, where every image is
   identical by construction. `olo_render_capture_target` and `olo_screenshot`
   report `ticking` / `stale` / `frameIndex`; assert on them per shot, and check
   the frame index advanced between shots. See
   [live-verification-noise-floor.md](live-verification-noise-floor.md).
3. **Decode the capture PNGs, do not look at them.** `GBufferAlbedo` *displayed*
   as solid white and was misread as "the G-Buffer is empty" — its alpha
   (metallic) is 0, so a viewer composites it against white. Counting unique
   values per channel instead gave the real answer immediately, and made the
   backend table decisive:

   | target | OpenGL | Vulkan |
   |---|---|---|
   | `GBufferAlbedo` (RT0) | 7 unique/channel | 7 — identical to GL |
   | `GBufferNormal` (RT1) | 256/188/5 | **1/1/1 — the clear value** |
   | `GBufferEmissive` (RT2) | 85/63/46 | **1/1/1 — the clear value** |
   | `SceneDepth` | 88 | 88 — identical to GL |

   RT0 and depth matching GL byte-for-byte is what rules out "the capture path
   resolves the wrong resource on Vulkan" — the same by-name view machinery
   answered correctly for attachment 0 and for depth in the same call sequence.
   Both halves of an "is my instrument lying?" question are answerable from one
   table if it contains a channel you already trust.

Three levers were also proven to *act* before anything was concluded from their
silence (the rule from
[render-graph-transient-aliasing.md](render-graph-transient-aliasing.md)): the
overdraw debug view and tonemap exposure both moved the frame and returned to
the baseline hash exactly, so "the light edits do nothing" is a statement about
lighting, not about a dead viewport.

## The fix, and its shape

`SetColorMask` fills `AttachmentColorMask`, and the pipeline lowering reads that
array as the **sole authority** on both the baked and the dynamic route — no
`& globalMask` composed back on top, because that would stop a
`glColorMaski`-shaped per-attachment WIDEN surviving a preceding global narrow.
One input decides, in both directions, as in GL.

**The blend twin looks identical and is not.** The obvious next step — make
`SetBlendState` fill `AttachmentBlend` too, and drop the `|| state.Blend` from
the lowering — was written, and the pass suite rejected it:
`DecalRenderPass` enables RT2 per-attachment for an Emissive decal whose
`PODRenderState` carries `blendEnabled = false`, so it needs the per-attachment
enable to survive the global disable `ApplyPODRenderState` then issues. GL would
clear it; the engine's passes are written against the OR. Matching GL there
would have deleted working additive decal accumulation on Vulkan *without*
fixing it on GL — a regression wearing parity's clothes. The change was reverted
and the divergence recorded at the declaration instead.

That is the part worth carrying away: **"same archetype" is a hypothesis, not a
licence.** Two adjacent pieces of state with the same GL rule can have different
callers depending on different semantics, and the only thing that separates them
is running the suite that covers the caller.

`RendererAPI` has exactly three indexed entry points —
`SetColorMaskForAttachment`, `SetBlendStateForAttachment`,
`SetBlendFuncForAttachment`. Two of the three globals now overwrite their array
(`SetColorMask`, and `SetBlendFunc` which already did); `SetBlendState`
deliberately does not, for the reason above.

Pinned by `VulkanDrawPath.GlobalColorMaskResetsPerAttachmentDivergence`,
which drives three render targets through **one recording** — because the leak
was process-scoped, a fresh frame per case would reset the very state under
test. It asserts that the indexed mask really masks, that a global call takes
it back, and that an indexed widen survives a global narrow. The first of those
is the control: a
no-op indexed mask would satisfy the reset assertion trivially, so without it
the test would pass on a broken build.

## What this surfaced next door, and did not fix

Restoring the GL semantic also takes away an accident Vulkan had been living
off. `DecalRenderPass::ExecuteOnGBuffer` sets **channel-level** per-attachment
masks (RT1.xy writable, RT1.zw preserved, and so on) and then calls
`CommandDispatch::InvalidateRenderStateCache()` so the next packet re-applies
its POD state — which runs `ApplyPODRenderState`, which calls the global
`SetColorMask`. On GL that has always wiped those masks before the decal draw;
on Vulkan they survived only because the global setter was inert. So the decal
mode matrix is defeated in production on **both** backends once this lands.

`VulkanPassSuite.DecalGBufferModeMatrixMasksItsTargetRenderTargets` does not see
it: the tenant substitutes `FixtureDecalDispatch`, which never applies POD
state, so it pins the *pass's* masking and not the path a real frame takes. A
per-draw contract verified through a dispatch function the production path does
not use is verifying the setup, not the draw.

Fixing it properly means the command layer owning per-attachment channel masks —
`PODRenderState::colorAttachmentWriteMask` is one bit per attachment and cannot
express them — so it is a separate change with its own tenant, filed rather than
folded in here.

## The checklist for the next facade entry point

- Does the GL entry point this mirrors have an **indexed sibling**? If so, decide
  explicitly whether it overwrites the indexed state, and write that decision
  into the array's declaration, not into the setter.
- Grep the **callers** for a comment that states the semantic. A caller that only
  ever narrows a state is depending on something to widen it.
- Ask which attachments a wrong answer would be **visible** on. State that only
  render target 0 exercises is state no screenshot gate covers, on any path that
  displays render target 0.
