# Every substitution a tenant makes is a seam it stops testing — and they compound

Postmortem of issue #853 (decal colour masks flattened by `ApplyPODRenderState`) and #1052 (virtual-geometry index buffer). Read before writing or reviewing a test that substitutes a production call, object or scene.

## Rules

1. **When a test substitutes a production seam, write down next to it which property it stops covering.** "One quad, not the production cube" says what; it must also say "so nothing here covers the cube's cull/depth pairing". The decal tenant's quad hid that `cullFace = Front` plus `depthFunction = LessOrEqual` rejected every fragment of a real decal cube. [Details](substituted-seams-compound-details.md#the-tenant-and-what-each-substitution-cost)
2. **Verify a per-draw contract through the production dispatch.** If the full dispatch is unreachable from the tenant (Renderer3D statics are live OpenGL objects mid-suite, so the Vulkan tenant cannot call `CommandDispatch::DrawDecal`), promote and call the specific production function that owns the contract, `CommandDispatch::ApplyPODRenderState`, and substitute only the demonstrably orthogonal parts. [Details](substituted-seams-compound-details.md#the-counter-moves)
3. **Build the state with the production builder.** `CreateDecalPODRenderState` is shared between `Renderer3D::DrawDecal` and the tenant so the tenant cannot pin its own copy of the contract. [Details](substituted-seams-compound-details.md#the-counter-moves)
4. **A feature with no scene has no coverage.** Author the missing scene first (one decal per mode on a uniform floor), then look at pixels. `TEST_SCENES.md` had no decal entry, which is why the render-graph hazard never fired: validation runs only when the graph's shape changes. [Details](substituted-seams-compound-details.md#why-nothing-else-caught-it-either)
5. **Check a hand-built graph or framebuffer against the real render graph.** The hand-built `GBuffer` plus separate depth framebuffer hid a `Read` of `SceneDepth` on the same framebuffer as the four written colour views, which trips an `OLO_CORE_ASSERT` (`__debugbreak`) and killed the editor on any Deferred scene with an opaque decal. [Details](substituted-seams-compound-details.md#the-tenant-and-what-each-substitution-cost)
6. **Do not trust an attachment-0 screenshot gate for G-Buffer work.** A defect confined to RT1/RT2 passes it; see [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md). Probe channels with `olo_render_probe_pixel` (no `target` returns every G-Buffer RT's raw texel) instead of eyeballing composites. [Details](substituted-seams-compound-details.md#the-measurement-order-that-worked)
7. **Measure cheapest-decisive first, and never conclude from silence.** Read the log the user reads (`GLStateGuard[...]: N state mutation(s) escaped the pass` showed the draw was issued and the fragments died); flip one variable as an experiment, not a fix (`LessOrEqual` to `GreaterOrEqual`) and look. [Details](substituted-seams-compound-details.md#the-measurement-order-that-worked)
8. **Assert the test count, not only the absence of failures.** A filter matching `Running 0 tests from 0 test suites` is not a pass; the contract tests had been added to `Rendering/CommandDispatchTest.cpp`, which is commented out of `tests/CMakeLists.txt`. An assertion abort (`FrameDataBufferManager not initialized!`) can leave no `[ FAILED ]` line, so grepping for FAILED finds nothing. [Details](substituted-seams-compound-details.md#the-two-green-looking-non-results-this-produced)
9. **A correct reading of a mechanism does not show the mechanism runs.** The mask leak was real (pinned by seven contract tests) but the draw it corrupted produced no fragments, so no capture could show it. Capture before claiming. [Details](substituted-seams-compound-details.md#what-the-issue-got-right-and-the-one-thing-it-could-not)
10. **A substitution also means "built the same object a different way": check against the production constructor, not the production type.** `VulkanPassSuite.VirtualGeometryMdiCountDrawsHandAuthoredClusters` used `IndexBuffer::Create(indices, 12)` (object-backed) while `VirtualMeshRegistry` uses `RenderCommand::CreateBufferHandle()` (raw handle, also bound as `SSBO_VIRTUAL_INDICES`). The raw handle was not in `VulkanRootObjectRegistry`, which `BindStorageBuffer` consulted, and lacked usage bits, so it bound null and every VG scene on Vulkan lost the device. Match factory, usage flags, registry and every binding point. [Details](substituted-seams-compound-details.md#second-instance-a-substituted-buffer-construction-1052)
11. **Assert `GetUnimplementedStubHitCount() == 0` in a Vulkan tenant,** as `VulkanDrawPathTest` does. `VulkanRendererAPI` counts unimplemented-stub hits; no virtual-geometry tenant asserted it. It also catches the next unlowered path. [Details](substituted-seams-compound-details.md#second-instance-a-substituted-buffer-construction-1052)
12. **Treat a test riding `RendererAttachedTest` as Vulkan coverage only when its suite runs in the `--olo-rhi=vulkan` process** (`OLO_VK_SCENE_FILTER` in `vulkan-software.yml`); the default process is GL only, and the virtual-geometry evidence is not in the Vulkan filter yet. [Details](substituted-seams-compound-details.md#second-instance-a-substituted-buffer-construction-1052)
13. **Prove which backend you are on.** `driver.ps1 -Action attach` takes `-Rhi vulkan` (it once launched OpenGL unconditionally); the only proof is `[RHI] Backend:` in `OloEditor/OloEngine.log`. See also [no-silent-fallbacks.md](no-silent-fallbacks.md), [testing-architecture.md](testing-architecture.md), [live-verification-noise-floor.md](live-verification-noise-floor.md). [Details](substituted-seams-compound-details.md#second-instance-a-substituted-buffer-construction-1052)

## Commands and checks

```
driver.ps1 -Action attach -Rhi vulkan
```

Starts the editor on Vulkan; confirm with `[RHI] Backend:` in `OloEditor/OloEngine.log`.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[substituted-seams-compound-details.md](substituted-seams-compound-details.md).
