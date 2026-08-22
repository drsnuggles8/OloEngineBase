# The observer camera: two cameras where the code assumed one (issue #726)

Applies to: any change that reads "the camera" inside culling, LOD selection,
Hi-Z occlusion, cascade fitting, or a GPU cull dispatch.

Freezing the culling camera is a two-line feature and a twenty-site audit. The
feature itself is one early-out (`Renderer3D::RefreshCullingCamera` stops copying
render → cull). Everything that made it hard is the second part: **every place
that reads "the camera" without saying which one is a latent lie**, and the lie
is not visible — the frame still renders, from a plausible cut that is not the
frozen one. That is strictly worse than having no tool, because the entire point
of this one is to be the ground truth you check other culling bugs against.

---

## 1. The one rule

> Everything that decides **what is drawn** reads the **culling** camera.
> Everything that decides **how it looks on screen** reads the **render** camera.

The culling camera is `Renderer3D::GetCull{View,Projection,ViewProjection}Matrix()`
/ `GetCullViewPosition()` / `GetCullNearClip()` / `GetCullFarClip()`. It equals the
render camera exactly until the freeze is toggled, so **the frozen path is the
only path** — there is no separate branch to rot. When you add a culling site,
reach for the `Cull*` accessor by default and justify the render camera, not the
other way round.

`RendererSettings::ObserverCameraEnabled` is the UI-facing bool (settings panel,
F3 overlay, MCP), but `Renderer3D::SetCullingCameraFrozen` is the **authority**:
it owns the freeze-time snapshot and writes the bool back.
`RefreshCullingCamera()` reconciles the two at the top of every frame, so a
toggle from any of the three UIs takes effect on the next frame whether or not
that UI called `ApplyRendererSettings()`.

---

## 2. The site enumeration

This is the list the design was derived from. An **unlisted site is a silent lie
later**, so extend it when you add one.

### Frozen (reads the culling camera)

| Site | File | What breaks if it follows the observer |
|---|---|---|
| CPU frustum cull | `RenderPipeline::PrepareFrame` → `data.ViewFrustum.Update(data.CullViewProjectionMatrix)` | The single biggest one — feeds `Renderer3DMeshSubmission`, `Renderer3DSpecializedDraws`, `IsVisibleInFrustum`, and the MCP `InFrustum` report |
| CPU frustum cull, worker threads | `ParallelSceneContext::ViewFrustum` | Same, on the parallel submission path |
| LOD selection (main thread) | `Renderer3DMeshSubmission.cpp` `SelectLODMesh(..., s_Data.CullViewPos, ...)` | Flying closer swaps in a finer mesh — the picture stops being the frozen one |
| LOD selection (workers) | `ParallelSceneContext::CullViewPosition` | Same |
| GPU instance frustum cull | `InstanceCullUBO::CullViewProjection`, read **unconditionally** by `InstanceFrustumCull.comp` / `InstanceOcclusionCull.comp` in place of the camera UBO | The GPU cull would follow the observer while the CPU cull stayed frozen — half a cut |
| Virtual-geometry cluster cull | `VirtualClusterCullUBO::CullViewProjection` / `CullCameraPosition` / `CullProjParams`, a **flagged** override in `VirtualClusterCull.comp` | Cluster LOD refines around the observer; the frustum and cone tests move |
| Hi-Z occlusion pyramid | `Renderer3D::GenerateOcclusionHZB` returns early while frozen | The pyramid would hold the observer's depth and occlusion-cull against surfaces the frozen camera never saw |
| Hi-Z reprojection matrix | `Renderer3DData::CullPrevViewProjectionMatrix` | Pyramid and matrix must describe the same instant or the occlusion test is garbage |
| Two-phase occlusion | disabled while frozen — `Renderer3DMeshSubmission.cpp` `hzbOcclusion`, `VirtualGeometryPass.cpp` `twoPhase` | Phase 2 rebuilds the pyramid **in place** from this frame's depth; while frozen that destroys the frozen pyramid phase 1 just tested against |
| CSM cascade fitting | `Scene::ProcessScene3DSharedLogic` → `ComputeCSMCascades(GetCullViewMatrix(), …)` | A cascade *is* a cull volume; #652 is exactly the class of bug this must be able to show |
| Terrain quadtree LOD + cull | `Scene::ProcessScene3DSharedLogic` → `MakeTerrainLocalCullInputs(…, cullCameraPosition, cullViewProjection)` (both the tessellated and the streamed-tile call sites) | Terrain silently re-tessellates around the observer |
| `OLO_DEBUG_SPACE_OBSERVER_NDC` | `RenderPipeline::UploadExecutionState` → `SetCameraState(debugVP, inverse(debugCullVP))` | This is the line #725 left behind for #726; a shader pushing in cull-camera NDC lands wrong without it |

### Deliberately NOT frozen (reads the render camera)

Say why, out loud, when you add one.

| Site | Why |
|---|---|
| The camera UBO (`ShaderBindingLayout::CameraUBO`) | It is the *render* camera by definition — every vertex stage feeds it to `gl_Position` |
| `Renderer3DData::RenderOrigin` | Camera-relative rendering (#429) exists for f32 precision **in the frame being drawn**. Both the instance transforms and the cull VP are shifted by the same origin, so the cull result is unchanged — see §3 |
| `PrevViewProjectionMatrix` | Motion-vector history. TAA and motion blur need it to keep tracking the observer; that is why the culling camera got its **own** `CullPrevViewProjectionMatrix` instead of freezing this one |
| Forward+ / clustered light culling | Light clusters are a *shading* acceleration structure for the pixels actually on screen. Freezing it would break lighting for the observer without telling you anything about geometry culling |
| Shadow-cascade **rendering** (`ShadowRenderPass`) and the virtual-geometry shadow cull | Those bind the *light's* matrices through the camera UBO. This is why `VirtualClusterCullUBO`'s override is flagged rather than unconditional: the ortho path leaves the value-initialised struct alone and reads the camera UBO exactly as before |
| Virtual-shadow-map page marking (`VirtualShadowMapMarkPass`) | It unprojects the **scene depth buffer**, which is the observer's depth. Its camera has to be the one that wrote those texels, or the clip-level rings shift relative to the pixels being shaded |
| Post-process depth reconstruction, decals, fog, SSR, water | All operate on the depth the observer just rendered |

---

## 3. Camera-relative rendering: two origins, one shift

`docs/agent-rules/camera-relative-rendering.md` says every world-space GPU upload
is a site, and with two cameras the obvious worry is two origins. There is only
one, and that is deliberate:

- The render origin keeps following the **render** camera, because it exists for
  f32 precision in the frame being drawn.
- The culling camera's matrices are made relative to **that same origin**
  (`CullViewProjectionRelative`, `CullViewPosRelative`, derived once in
  `PrepareFrame`), because the instance transforms the GPU cull reads were
  already shifted by it.

Shifting both sides of a cull test by the same vector leaves the result
unchanged, so freezing costs no precision — but deriving the relative forms *per
dispatch* instead of once would let three cull shaders disagree about which
origin they were shifted by. They are derived centrally for that reason.

---

## 4. Things that cost debugging time

**The frustum wireframe needs `ShaderDebugDrawEnabled`.** The pass that consumes
the channel is not even *declared* while shader debug draws are off, so the
frustum silently never appears. Both UIs say so inline and the MCP reply carries
`shaderDebugDrawEnabled` for the same reason. Before concluding "the frustum drew
nothing", read the two-counter overflow protocol in
[gpu-debug-draws.md](gpu-debug-draws.md) §2b — an overflow and a no-op look
identical on screen.

**Freeze first, pose second.** `SetCullingCameraFrozen(true)` snapshots the
camera *at the instant of the call*, not at the next frame. An MCP session that
poses the camera and then freezes has frozen the wrong thing, and the result
looks entirely plausible.

**A frozen frame is stale by design, and there is no timeout.** Every later
screenshot keeps showing the frozen cut. Unfreeze when done; the MCP tool's
description says so because forgetting produces a stream of confusing captures
much later.

**Corner order is a contract.** `Renderer3D::ComputeFrustumCorners` returns
corners indexed `bit0 = +x, bit1 = +y, bit2 = +z` in NDC, because
`ShaderDebugDrawBox`'s expansion recovers the 12 edges as "every pair differing
in exactly one bit". Any other order draws a tangle rather than a frustum, and it
still looks like *something*, which is the slow way to find out.

**The corners come from `inverse(viewProjection)`, not from fov/aspect/near/far.**
The culling camera's projection is whatever the scene camera handed us —
perspective, orthographic, or an asymmetric off-centre frustum from a reflection
or a portal — and the only description guaranteed to match what
`Frustum::Update()` actually culled with is the matrix itself.
`Renderer3D::DrawCameraFrustum` (the older entity gizmo) rebuilds from
fov/aspect *and clamps the far plane to 50 units for legibility*; it is a gizmo,
not an instrument, and must not be used to draw the frozen frustum.

**Two-phase occlusion is off while frozen, and that is not a limitation to
"fix".** Phase 2's job is to *un*-cull instances phase 1 deferred, using a
pyramid rebuilt from this frame's depth. While frozen that depth is the
observer's. Single-phase against the retained frozen pyramid is the honest
answer: an instance the frozen camera occluded stays occluded, which is the
property the whole feature rests on. `BuildCurrentOcclusionHZB` also refuses
outright while frozen, as a backstop, because that rebuild is in-place.

---

## 5. Where the guards are

| What | Where |
|---|---|
| Freeze/mirror state machine, settings-bool sync | `OloEngine/tests/Rendering/ObserverCameraTest.cpp` |
| Frustum-corner unprojection + corner-bit order | same file |
| "the two frustums genuinely disagree" property | same file |
| Frozen cut visibly missing geometry from the observer; centre band unchanged; unfreeze restores | `OloEngine/tests/Rendering/PropertyTests/ObserverCameraVisualEvidenceTest.cpp` |
| Frozen-frustum wireframe actually draws | same file |
| The GPU **compute** cull honours the frozen camera (>1024 instances, so the batch really routes through `SubmitGPUCulledInstanced`) | `ObserverCameraGpuCullEvidenceTest`, same file |
| `InstanceCullUBO` / `VirtualClusterCullUBO` std140 sizes vs their GLSL twins | `static_assert`s in `ShaderBindingLayout.h` |

**`GPUFrustumCullParityTest` does not guard this.** It re-implements
`InstanceFrustumCull.comp`'s arithmetic in C++ and never dispatches the shader, so
it cannot see a std140 offset that shifted or a new field nobody filled. That is
why the guard above is a full-pipeline test with a batch deliberately over the
GPU-cull threshold -- at or below 1024 instances the batch silently takes the CPU
path and such a test passes while proving nothing.

---

## 6. Driving it from an agent session

```
olo_shader_debug_draw   { enabled: true }        # or the frustum will not draw
olo_camera_set_pose     { position: [...], target: [...] }   # the pose to freeze
olo_camera_freeze_culling { frozen: true }       # snapshots THIS pose
olo_camera_orbit        { target: [...], distance: 80 }      # fly out
olo_screenshot          { }
olo_camera_freeze_culling { frozen: false }      # do not leave it frozen
```

`olo_camera_freeze_culling` returns `cullFrustumCorners` — the 8 world-space
corners — so an agent can pick an observer pose that actually contains the frozen
volume instead of guessing.
