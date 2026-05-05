# RenderGraph Rework — Ghosting Investigation

> Branch: `feature/rendergraph_rework`
> Symptom: persistent "ghost" duplicates of scene objects visible in the editor
> viewport (see attachment in the original report). Mountains/water/foreground
> primitives render normally; large translucent sphere shapes appear floating
> in the upper portion of the frame.

## 1. Suspect set (initial triage)

The branch contains 3 commits totalling ~9.7k line diff. Render-side hot spots
relevant to the ghost-trail symptom:

| Subsystem | Why it could cause ghosting |
| --- | --- |
| **TAA** (`PostProcessRenderPass.cpp`) | Reads `m_TAAHistoryFB` blended with current frame — wrong history → multi-image accumulation. New code resolves velocity / depth via `RGCommandContext` instead of direct member pointers. |
| **PostProcess output** (new `m_OutputFB` + `ResolveToOutput`) | Stable handoff target; if `currentSource` ever points at the wrong FB, downstream readers see stale content. |
| **Bloom** | Mip chain + additive blends at the end — stale mip content can produce wide halos. |
| **Volumetric fog history** (`m_FogHistoryFB`) | Temporal reprojection of fog inscatter — broken depth / camera matrices can leave persistent stripes. |
| **OIT accum/revealage** (`OITBuffer::ClearForFrame`) | If accumulation buffer isn't cleared each frame, transparent geometry stacks up across frames. WaterRenderPass currently forces `useOIT = false` (intentional). |
| **Velocity buffer import** (`SetupFrameBlackboard`) | `board.Velocity = scene FB attachment 3` in forward, GBuffer velocity attachment in deferred. If the wrong texture ID is imported, TAA reads garbage motion and produces persistent disocclusion ghosts. |

## 2. Architecture / data-flow checks done

* `RenderGraph::Execute` (`RenderGraph.cpp:836-867`) constructs a single
  `RGCommandContext`, calls `SetRenderGraph(this)`, then dispatches
  `pass->Execute(context)`. The legacy `Execute()` overloads create a
  *default* `RGCommandContext` with `m_RenderGraph == nullptr`, so any pass
  invoked outside the graph cannot resolve handles. **Inside the graph this
  path is fine.**
* `Renderer3D::SetupFrameBlackboard()` (`Renderer3D.cpp:967-1090`) imports
  scene depth, velocity, normals, AO, shadow maps. Velocity in forward path
  is imported from `s_Data.ScenePass->GetTarget()->GetColorAttachmentRendererID(3)`.
  In `SceneRenderPass::Execute`, attachment 3 is cleared to `(0,0)` only when
  `attachments[3].TextureFormat == RG16F`. Need to verify the scene FB really
  has a 4-attachment layout in the active mode (Forward+ vs Deferred).
* `PostProcessRenderPass::Execute(RGCommandContext&)` resolves
  `m_SceneDepthHandle`, `m_AOTextureHandle`, `m_ShadowMapCSMHandle`,
  `m_VelocityTextureHandle` from the graph. Falls back to
  `inputFramebuffer->GetDepthAttachmentRendererID()` if scene depth resolves
  to 0. Velocity has no fallback — if the handle resolves to 0, TAA runs in
  "camera-only reprojection" mode.
* `PostProcessRenderPass::GetTarget()` now always returns `m_OutputFB` (was
  ping-pong). `ResolveToOutput` blits final `currentSource` into `m_OutputFB`
  every frame. **`m_OutputFB` does NOT receive depth — only color blit.**
* `WaterRenderPass`: `useOIT = false` is forced — water always uses forward
  path. OIT path therefore not active for water.
* TAA history blit (`PostProcessRenderPass.cpp:404-412`) writes `dest` →
  `m_TAAHistoryFB`. `m_TAAHistoryValid` is correctly invalidated when TAA
  shader / history FB / scene depth are missing.
* Camera UBO (`PostProcess UBO binding 7`) is rebound after `SetData()` —
  good (matches earlier IBL UBO rebind fix).

## 3. Most likely root causes (ranked)

1. **TAA history retains content from a stale framebuffer ID.** `m_OutputFB`
   was added; if any scene-FB resize path forgot to invalidate
   `m_TAAHistoryValid`, the history will sample a deleted/recycled GL texture
   and produce arbitrary trails. *(Resize path explicitly clears it — looks
   OK; cross-check at runtime.)*
2. **Velocity texture handle resolves to wrong attachment.** In forward mode,
   `SetupFrameBlackboard` reads scene FB attachment 3. If the scene FB
   attachment list isn't 4 entries (e.g. Forward+ without velocity slot), this
   imports the depth or another buffer as velocity, sending bogus motion
   vectors into the TAA shader.
3. **Effect chain ordering after `m_OutputFB` introduction.** Tone-map runs
   late; `currentSource` is whatever the last effect wrote. If TAA is enabled
   but tone-map isn't or vice-versa, `ResolveToOutput` may copy the wrong
   intermediate FB.

## 4. What I attempted

* Read the full diff of `PostProcessRenderPass.cpp`, `Renderer3D.cpp`
  (blackboard setup + post-process wiring), `OITResolveRenderPass`,
  `WaterRenderPass`, `FinalRenderPass`, `UICompositeRenderPass`,
  `SceneRenderPass`, `FrameBlackboard.h`, `RGCommandContext.{h,cpp}`.
* Verified the editor exposes a **`Render Graph Debugger`** panel
  (`EditorLayer::m_ShowRenderGraphDebugger`) — toggle from the menu bar; it
  visualises the live graph topology, hazard validation results, and per-pass
  timings.
* Verified `GPUResourceInspector` and `CommandPacketDebugger` panels exist
  too (`EditorLayer.cpp:1358-1372`).

## 5. Next steps for the user (manual triage)

Run the editor (Debug build) and try the following in order — they are
ordered to pin the broken stage with the fewest restarts:

1. **Open the Render Graph Debugger** (menu → Render Graph Debugger). Confirm
   pass list matches expectations and there are no red hazards on
   `PostProcessRenderPass`, `SSAO/GTAO`, `OITResolveRenderPass`.
2. **Open the GPU Resource Inspector**. Find `_VelocityBuffer` /
   `OloEngine::ResourceNames::Velocity`. Confirm:
   * It has a non-zero texture ID.
   * Preview shows a mostly-black image (zero motion for a static camera).
     If the preview shows scene depth, normals, or color → wrong attachment
     imported.
3. **Toggle TAA off** in the Post-Process panel (`Renderer Stats` /
   `Post-Process`). If the ghosts vanish → TAA is the culprit (history /
   velocity issue).
4. **If still ghosts:** toggle Bloom off, then Fog off, then Motion Blur off,
   then DOF off. Whichever toggle removes the ghosts identifies the bad
   stage.
5. **If still ghosts with all post-process disabled:** the issue is upstream
   (Scene/Forward overlay/Water/Decal). Disable Water (delete water entity or
   set its material `Enabled = false`) to test. Otherwise inspect the
   `_SceneColor` framebuffer in the GPU Resource Inspector frame-over-frame.

## 6. Hypothesis log (update as we test)

| Hypothesis | Status | Notes |
| --- | --- | --- |
| TAA history sampling stale FB ID | **eliminated** | TAA + all post-fx off → ghosts persist |
| Wrong velocity texture imported in forward mode | **eliminated** | no `_VelocityBuffer` in inspector (forward path) |
| Bloom mip chain not reset between resizes | **eliminated** | bloom off → ghosts persist |
| Fog history reprojection broken (wrong depth handle) | **eliminated** | fog off → ghosts persist |
| OIT accum not cleared (water OIT forced off) | **eliminated** | no water entity in scene |
| RenderGraph::Execute double-dispatching passes | **eliminated** | line 856 single `pass->Execute()` per cached order entry |
| Scene::RenderScene3D submitting each entity twice | **eliminated** | one `Renderer3D::DrawMesh` + `SubmitPacket` per entity |
| Selection outline JFA re-rendering geometry | **eliminated** | JFA only seeds from scene-FB attachment 1 (entity IDs), no mesh redraw |
| ShadowRenderPass leaking into scene FB | **eliminated** | binds `m_ShadowFramebuffer` correctly, color mask off, restores state |
| LightProbeBaker re-rendering scene per face | **eliminated** | no probe entities in `LightingTest.olo` |
| Depth prepass running color writes (SceneRenderPass executes bucket twice when prepass enabled) | **open** | bucket.Execute is called twice (lines 184 + 228); if `SetDepthPrepassActive` no longer suppresses color writes, both Executes draw color — would explain ghosting if combined with state leak that offsets second draw |
| `PostProcessRenderPass::ResolveToOutput` mis-blit (size mismatch) | **open** | new `m_OutputFB` with `glBlitNamedFramebuffer`; size check needed |
| Reflection probe / planar reflection capture writing to scene FB | **eliminated** | no probes in scene, IBL diff is 2 lines |
| Temporal occlusion culling proxy boxes leaking color writes | **open** | 1:1 with entities, path-agnostic, prepass-agnostic, shadow-agnostic |
| Editor gizmo path drawing every entity's bounding sphere | **open** | path-agnostic, but should not be 1:1 with mesh entities |

## 7. Key facts established (do not re-investigate)

- 1:1 entity-to-ghost mapping (deletion test) → geometry-driven duplication
- All post-process effects can be off and ghosts remain → cause is upstream of `PostProcessRenderPass`
- Ghosts shifted in screen-Y from the originals → there is a transform offset between the two renders, NOT just a stencil/blend artifact
- Single-threaded scene mesh iteration in `Scene::RenderScene3D` (Scene.cpp:3712) draws each `MeshComponent` entity exactly once via `DrawMesh→SubmitPacket→ScenePass`

## 8. Recommended next bisect (asks for the user)

Run these one at a time and note which one removes the ghosts:

1. **Disable Depth Prepass** (Renderer Settings → uncheck `Depth Prepass`).
   - If ghosts vanish: the depth prepass is now writing color (regression in
     `CommandDispatch::SetDepthPrepassActive`).
2. **Disable shadows on the directional light** (or set
   `m_CastShadows = false` on the light component).
   - If ghosts vanish: `ShadowRenderPass` is leaking into the scene FB
     (despite the static binding looking correct — could be that the new
     `RGCommandContext::BeginPass` doesn't restore the FB before `Bind()`).
3. **Switch RenderingPath to Deferred** in renderer settings.
   - If ghosts vanish in deferred: a forward-path-specific double-render.
   - If ghosts appear in deferred too: an upstream pass common to both paths.
4. **Open Render Graph Debugger** → find the row labeled `ScenePass` → look
   at "FB writes" column. If something other than `_SceneColor` shows up, or
   if `_SceneColor` is listed under another pass that isn't supposed to
   write it, that's the offending pass.
5. **Toggle Forward+ off** (Renderer Settings → disable Forward+ light culling).
   - If ghosts vanish: the Forward+ debug heatmap or culling overlay is
     drawing into the wrong FB.

Add results back here once the user has triaged the toggles.
