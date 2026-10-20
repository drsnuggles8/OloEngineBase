# The observer camera: two cameras where the code assumed one (issue #726)

Applies to: any change that reads "the camera" inside culling, LOD selection, Hi-Z occlusion, cascade fitting, or a GPU cull dispatch. An unlisted camera-reading site is a silent lie later, because the frame still renders from a plausible cut that is not the frozen one.

## Rules

1. **Read the culling camera for anything that decides what is drawn, and the render camera for how it looks.** Use `Renderer3D::GetCull{View,Projection,ViewProjection}Matrix()`, `GetCullViewPosition()`, `GetCullNearClip()`, `GetCullFarClip()`; they equal the render camera until frozen, so there is no separate branch. Default to `Cull*` and justify the render camera. [Details](observer-camera-details.md#1-the-one-rule)
2. **Treat `Renderer3D::SetCullingCameraFrozen` as the authority.** It owns the snapshot and writes `RendererSettings::ObserverCameraEnabled` back; `RefreshCullingCamera()` reconciles them at the top of every frame. [Details](observer-camera-details.md#1-the-one-rule)
3. **Use the culling camera at these sites, and extend the list when you add one.** CPU frustum (`RenderPipeline::PrepareFrame`, `ViewFrustum.Update(data.CullViewProjectionMatrix)`); `ParallelSceneContext::ViewFrustum` and `CullViewPosition`; LOD selection (`SelectLODMesh(..., s_Data.CullViewPos, ...)`); `InstanceCullUBO::CullViewProjection` (read unconditionally by `InstanceFrustumCull.comp` / `InstanceOcclusionCull.comp`); `VirtualClusterCullUBO` `CullViewProjection` / `CullCameraPosition` / `CullProjParams` (a flagged override in `VirtualClusterCull.comp`); `Renderer3D::GenerateOcclusionHZB` (returns early while frozen); `Renderer3DData::CullPrevViewProjectionMatrix`; CSM cascade fitting (`ComputeCSMCascades(GetCullViewMatrix(), ...)`); terrain quadtree LOD and cull (`MakeTerrainLocalCullInputs`, tessellated and streamed-tile sites); `OLO_DEBUG_SPACE_OBSERVER_NDC` in `RenderPipeline::UploadExecutionState`. [Details](observer-camera-details.md#frozen-reads-the-culling-camera)
4. **Keep two-phase occlusion off while frozen.** Phase 2 rebuilds the pyramid in place from this frame's (observer) depth and would destroy the frozen pyramid; single-phase against the frozen pyramid is correct, and `BuildCurrentOcclusionHZB` refuses while frozen as a backstop. Do not "fix" this. [Details](observer-camera-details.md#frozen-reads-the-culling-camera)
5. **Keep these on the render camera and say why when adding one.** Camera UBO (`ShaderBindingLayout::CameraUBO`); `Renderer3DData::RenderOrigin`; `PrevViewProjectionMatrix` (hence the separate `CullPrevViewProjectionMatrix`); Forward+/clustered light culling; shadow-cascade rendering (`ShadowRenderPass`) and the virtual-geometry shadow cull (which is why the `VirtualClusterCullUBO` override is flagged); `VirtualShadowMapMarkPass` (unprojects observer depth); post-process depth reconstruction, decals, fog, SSR, water. [Details](observer-camera-details.md#deliberately-not-frozen-reads-the-render-camera)
6. **Keep one render origin and derive the relative cull forms once.** The origin follows the render camera; `CullViewProjectionRelative` and `CullViewPosRelative` are derived once in `PrepareFrame` so the three cull shaders cannot disagree on the shift. See `camera-relative-rendering.md`. [Details](observer-camera-details.md#3-camera-relative-rendering-two-origins-one-shift)
7. **Enable `ShaderDebugDrawEnabled` before expecting the frustum wireframe.** The consuming pass is not declared otherwise; the MCP reply carries `shaderDebugDrawEnabled`. Read the overflow protocol in [gpu-debug-draws.md](gpu-debug-draws.md) §2b before concluding it drew nothing. [Details](observer-camera-details.md#4-things-that-cost-debugging-time)
8. **Freeze first, pose second, and unfreeze when done.** `SetCullingCameraFrozen(true)` snapshots at the instant of the call; there is no timeout and later screenshots keep the frozen cut. [Details](observer-camera-details.md#4-things-that-cost-debugging-time)
9. **Keep frustum corner order `bit0 = +x, bit1 = +y, bit2 = +z` in NDC.** `Renderer3D::ComputeFrustumCorners` order is a contract with `ShaderDebugDrawBox` (12 edges = pairs differing in one bit). [Details](observer-camera-details.md#4-things-that-cost-debugging-time)
10. **Derive corners from `inverse(viewProjection)`, not fov/aspect/near/far.** The projection may be orthographic or asymmetric. `Renderer3D::DrawCameraFrustum` clamps far to 50 units and is a gizmo, not an instrument. [Details](observer-camera-details.md#4-things-that-cost-debugging-time)
11. **Guard with a full-pipeline test, over 1024 instances.** `GPUFrustumCullParityTest` re-implements the arithmetic and never dispatches the shader; `ObserverCameraGpuCullEvidenceTest` uses a batch above the threshold so it routes through `SubmitGPUCulledInstanced` (at or below 1024 it silently takes the CPU path). Other guards: `ObserverCameraTest.cpp`, `ObserverCameraVisualEvidenceTest.cpp`, and the `InstanceCullUBO` / `VirtualClusterCullUBO` std140 `static_assert`s in `ShaderBindingLayout.h`. [Details](observer-camera-details.md#5-where-the-guards-are)

## Commands and checks

```
olo_shader_debug_draw   { enabled: true }        # or the frustum will not draw
olo_camera_set_pose     { position: [...], target: [...] }   # the pose to freeze
olo_camera_freeze_culling { frozen: true }       # snapshots THIS pose
olo_camera_orbit        { target: [...], distance: 80 }      # fly out
olo_screenshot          { }
olo_camera_freeze_culling { frozen: false }      # do not leave it frozen
```

`olo_camera_freeze_culling` returns `cullFrustumCorners` (8 world-space corners) to pick an observer pose that contains the frozen volume.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[observer-camera-details.md](observer-camera-details.md).
