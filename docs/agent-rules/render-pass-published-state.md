# A pass that publishes engine-global bindings must restore state deliberately — GLStateGuard(Restore) will undo the publication

Lessons from the DDGI probe pass bring-up (#632). They apply to any
render-graph pass whose outputs are consumed OUTSIDE the graph's resource
tracking — i.e. anything following the `VolumetricFogPass` /
`SetGlobalIBL` pattern of binding results at engine-reserved sampler
slots for later passes to sample.

## 1. Do not wrap such a pass's Execute in GLStateGuard(Restore)

`GLStateGuard(Policy::Restore)` snapshots GL state at construction and
restores it at scope exit. For a pass whose *intended contract* includes
leaving textures bound at engine-global slots (DDGI's atlases at
`TEX_DDGI_*`, fog's integrated volume at `TEX_FROXEL_FOG`), the guard's
exit restore **reverts the publication to the previous frame's
bindings** — the frame keeps "working" one frame stale, which is nearly
invisible and therefore worse than breaking outright. The guard also
logs every escaped mutation at trace level: a pass that legitimately
mutates state under the guard floods `OloEngine.log` at ~90 lines/frame
("N state mutation(s) escaped the pass (restoring)").

The correct shape (see `DDGIProbeUpdatePass::Execute`):

1. Mutate freely through the pass's stages.
2. **Deliberate restore block** at the end: unbind the FBO, restore
   depth/cull to the scene defaults, unbind pass-local texture units
   (§6.4 hygiene — soon-recreated textures must not dangle on live
   units), restore the viewport, re-establish shared UBOs.
3. **Publish LAST**, after the restore block, so nothing reverts it.

GLStateGuard(Restore) remains correct where it is already used: test
fixtures (`RendererAttachedTest` per-tick) and clear-helper wrappers —
scopes whose contract is "no state change escapes".

## 2. `Shader::Bind()` can re-bind UBO binding points behind your back

The shader resource registry re-binds registered uniform blocks when a
program is bound. Practical consequence: a pass that overwrites a shared
UBO binding point (e.g. the camera UBO at binding 0 with a per-face
capture camera) and then restores it mid-Execute can have the restore
silently undone by a LATER `Shader::Bind()` in the same Execute.
**Re-establish shared UBO bindings at the END of the pass, after the
last shader bind** — and call `CommandDispatch::InvalidateRenderStateCache()`
first, or `BindUBOIfNeeded` will think the binding never changed and
skip the re-bind.

## 3. Amortized-refresh schedulers need a steady-state cap

A "fill the remaining budget with refresh work" scheduler degenerates to
re-doing the FULL budget every frame forever once the initial fill
completes (measured: 9.7 ms/frame GPU that should have been ~2.5 ms).
Steady-state refresh belongs at a FRACTION of the budget (Lumen uses
1/8 — `CardCaptureRefreshFraction = 0.125`); reserve the full budget for
genuinely new/invalidated work. See `DDGIProbeUpdatePass::PickCaptureSet`.

## 4. Scene-authoring trap: `MeshComponent { Primitive: 0 }` is None

`MeshPrimitive::None == 0`, so a scene YAML `Primitive: 0` produces an
entity that never renders on any path — no error, no warning. (The
LightProbesTest sandbox scene shipped three such "spheres" that had never
rendered; Sphere is 2.) When a mesh entity mysteriously contributes
nothing — no draw, no shadow, no GI — check the primitive enum value
before suspecting the renderer.

## 5. Pass-owned textures: import from Setup, hash the ids, stable ping names

To make a pass-owned raw GL texture visible to the render graph (and so to
`olo_render_list_targets` / `olo_render_capture_target`), `ImportTexture` it
from the pass's **`Setup()`** (never Execute — the resource must be resolvable
the same rebuild that registers it) and hash **both the gate and the raw
texture ids** into `ComputeBlackboardFingerprint`, because imports are wiped by
every non-cached `PopulateBlackboard` and Setup only reruns on a fingerprint
miss. Two refinements learned in the #607 batch (DDGI atlases, froxel-fog
volumes; precedents: VirtualGeometryDebug, FluidIntermediates):

- **Ping-pong resources import BOTH pings under stable per-ping names**
  (`DDGIIrradianceAtlas0/1`). Importing "the current ping" would flip the
  fingerprint every blended frame and defeat the BuildFrameGraph cache.
- A genuine 3D volume imports as `ResourceHandle::Kind::Texture3D` with
  `DepthOrLayers = depth`; the capture path already reads one z-slice via its
  `layer` selector.

## 6. Post-pass hooks are keyed and multi-listener; snapshots must copy at hook time

`RenderGraph::AddPostPassHook(key, fn)` / `RemovePostPassHook(key)` replaced
the single-slot `SetPostPassHook` (#607): the debugger's frame capture
(`"framecapture"`) and the MCP afterPass snapshot (`"mcp-afterpass-snapshot"`)
coexist on the same graph. Never re-introduce a single-slot install, and never
use the graph's `HasPostPassHook()` to test whether *your* hook is installed —
it reports ANY listener (`RenderGraphFrameCapture::IsHookInstalled` exists for
that). A mid-frame snapshot must `glCopyImageSubData` the resolved texture
INSIDE the hook, not remember the GL id: the transient pool memory-aliases
same-descriptor resources, so a mid-frame id can be recycled for a different
logical resource later in the same frame. `glCopyImageSubData` binds nothing,
so the publish-last rules of §1 are unaffected.
