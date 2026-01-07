## Plan: Enable Switchable 3D Rendering in OloEditor (Final)

Add a 2D/3D renderer mode toggle to OloEditor with entity-picking support via an extended RenderGraph. Lights become ECS components, requiring Renderer3D and shader updates to collect lights from the scene. Use uniform-per-draw for entity IDs, default materials for meshes without explicit assignment, and warn when light limits are exceeded.
---

### Steps

**Phase 1: Core 3D Rendering Pipeline (MVP)**

1. **Add `Renderer3D::BeginScene(EditorCamera)` overload** - Extend [Renderer3D.h](OloEngine/src/OloEngine/Renderer/Renderer3D.h) and [Renderer3D.cpp](OloEngine/src/OloEngine/Renderer/Renderer3D.cpp) to extract view/projection from `EditorCamera` and set up camera UBO.

2. **Add entity-ID pass to RenderGraph** - Extend [RenderGraph.cpp](OloEngine/src/OloEngine/Renderer/RenderGraph.cpp) and `SceneRenderPass` to include a `RED_INTEGER` attachment; add `int u_EntityID` uniform to 3D shaders and output to this attachment (TODO: extend to instancing later).

3. **Implement `Scene::RenderScene3D(EditorCamera)`** - Add method in [Scene.cpp](OloEngine/src/OloEngine/Scene/Scene.cpp) that calls `Renderer3D::BeginScene()`, iterates `MeshComponent`/`SubmeshComponent` entities, passes entity ID per draw call, and ends scene.

4. **Add 2D/3D mode toggle in EditorLayer** - Add `m_Is3DMode` flag and toolbar button in [EditorLayer.cpp](OloEditor/src/EditorLayer.cpp); call `RenderScene()` or `RenderScene3D()` based on mode; read entity ID from Renderer3D's framebuffer for mouse picking.

**Phase 2: Light Components & Renderer Integration**

5. **Create light component structs** - Add `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent` in [Components.h](OloEngine/src/OloEngine/Scene/Components.h) with color, intensity, direction, attenuation, cutoff properties.

6. **Update Renderer3D to collect lights from Scene** - Add `Renderer3D::SetSceneLights(Scene*)` or integrate into `BeginScene`; query light component entities, populate multi-light UBO; add warning log when exceeding max light count (e.g., 16 point lights).

7. **Update 3D shaders for entity-ID and multi-light** - Add `uniform int u_EntityID` and second render target output to [PBR.glsl](OloEditor/assets/shaders/PBR.glsl), [Lighting3D.glsl](OloEditor/assets/shaders/Lighting3D.glsl), and skinned variants; ensure multi-light UBO is read correctly.

**Phase 3: Component UI (Essential)**

8. **Add MeshComponent UI** - In [SceneHierarchyPanel.cpp](OloEditor/src/Panels/SceneHierarchyPanel.cpp), add `DisplayAddComponentEntry<MeshComponent>` with asset picker for `MeshSource` handle.

9. **Add MaterialComponent UI** - Add full PBR property editor (albedo, metallic, roughness, AO, emission, texture slots); when `MeshComponent` is added without `MaterialComponent`, auto-assign default gray material.

10. **Add light component UI** - Add entries for `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent` with color picker, intensity slider, direction/position, attenuation controls.

11. **Add 3D physics component UI** - Add entries for `Rigidbody3DComponent`, `BoxCollider3DComponent`, `SphereCollider3DComponent`, `CapsuleCollider3DComponent` with property editors.

**Phase 4: Serialization (Essential)**

12. **Serialize MeshComponent** - In [SceneSerializer.cpp](OloEngine/src/OloEngine/Scene/SceneSerializer.cpp), save/load `MeshSource` asset handle.

13. **Serialize MaterialComponent** - Save/load all PBR properties and texture asset handles; add YAML converters in [YAMLConverters.h](OloEngine/src/OloEngine/Core/YAMLConverters.h) as needed.

14. **Serialize light components** - Save/load `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent` properties.

15. **Serialize 3D physics components** - Save/load `Rigidbody3DComponent`, collider components with dimensions, offsets, physics material.

---

### Deferred to Later Phases

- Animation panel (timeline, playback, clip browser)
- Skeleton/bone debug visualization
- ContentBrowserPanel 3D extensions (model icons, drag-drop, thumbnails)
- Environment settings (skybox, HDR, ambient panel)
- 3D collider gizmo wireframes
- Entity-ID instancing optimization

---

### Summary of Decisions

| Decision | Choice |
|----------|--------|
| Entity-ID approach | Uniform per draw (`u_EntityID`), TODO instancing |
| Missing MaterialComponent | Auto-assign default gray material |
| Exceeding light limits | Log warning, clamp to max |
