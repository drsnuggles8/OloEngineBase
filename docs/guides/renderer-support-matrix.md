# Renderer geometry coverage

The executable decision contract is `Renderer/Support/RendererSupport.h`. The
42 cases in `Renderer/Support/RendererSupportRows.h` pin representative
geometry, material, backend, path, sample-count and upscale combinations.
An outcome describes the **requested combination**, not whether a related
implementation exists elsewhere. These rows were checked against source and
existing tests on 2026-09-23; this page does not claim a new test or live run.

| Requested combination | Outcome | Reason and current evidence |
|---|---|---|
| Virtual static mesh, opaque, deferred raster | Supported | `VirtualMeshRegistry::PrepareFrame` submits per-part materials and cluster draws. `VirtualGeometryVisualEvidenceTest.cpp` covers the hardware path, shadows, MSAA, culling and residency. |
| Virtual skinned mesh, opaque, deferred raster | Supported | `Renderer3DMeshSubmission.cpp` supplies current and previous bone palettes. `VirtualGeometryVisualEvidenceTest.cpp` covers deformation from multiple angles and disk-backed pages. A cook without skin bindings instead renders the rest pose and emits a warning. |
| Virtual static mesh, masked, raster shadows | Approximate | `VirtualMeshRegistry.cpp` suppresses shadow casting for alpha-masked parts because the shadow-depth shader does not sample alpha (`PrepareFrame`, near the `CastShadows` assignment). The visible cutout still uses hardware raster. |
| Virtual mesh, blended material | Unsupported | `VirtualMeshRegistry::PrepareFrame` skips `AlphaMode::Blend`; its deferred G-buffer stores only one opaque surface. `VirtualGeometryVisualEvidenceTest.cpp::ABlendPartTheRasterPathRefusesIsNotTracedEither` checks the paired ray-space refusal. Use a classic mesh for transparent rendering. |
| Virtual static mesh, Vulkan ray query | Approximate | `Renderer3DMeshSubmission.cpp::StageVirtualProxy` supplies a fixed, coarse DAG cut to the GPU Scene. `VirtualGeometryVisualEvidenceTest.cpp::VirtualMeshReachesGPUSceneAsARayTracingProxy` checks extraction. The proxy is not the raster cut or a deforming surface. |
| Virtual skinned mesh, ray query or path tracing | Unsupported | `Renderer3DMeshSubmission.cpp` refuses to stage its fixed rest-pose proxy for a skinned part; `ProxylessParts` and `GPUSceneUnsupportedCategory::Virtualized` count the refusal. |
| Deforming classic skinned mesh, ray query, deformed vertex stream missing | Unsupported | `RayTracingScene::Classify` refuses the animated instance and increments `AnimatedInstancesRefused`; tracing its rest pose would produce wrong shadows. |
| Deforming classic skinned mesh, ray query, deformed vertex stream ready | Supported | `RayTracingScene::Classify` admits the per-instance deformed stream for refit. The support request must supply that stream observation; device ray-query support alone is insufficient. |
| Virtual morph target | Unsupported | `VirtualMeshBuilder::BuildSet` rejects sources with morph targets (`VirtualMeshBuilder.cpp`). `Request::MorphTargets` records this independently of skinning, material and transmission. |
| Virtual WPO (#1152), tessellation/displacement (#1153), spline/landscape (#1154), programmable raster (#1155) | Unsupported | The virtual-geometry builder, registry and virtual raster shaders have no implementation for these families. Classic terrain and water tessellation are separate representations and do not satisfy a virtual-geometry request. |
| Gaussian splat as a production scene entity (#1046) | Unsupported | `Renderer/Splat` has PLY decode, ordering and LOD code, and `GaussianSplatVisualEvidenceTest.cpp` renders an isolated GL fixture. There is no splat scene component, asset route or render-pipeline pass. |
| Groom raster strands without scene shadow | Supported | `GroomRenderPass` draws strands and `GroomStrand.glsl` samples the coat's internal optical-depth volume. This row makes no scene-shadow claim. |
| Groom raster scene-shadow casting or receiving (#1323) | Unsupported | `ShadowRenderPass` has no groom caster family; `GroomStrand.glsl` does not sample scene shadow maps. The coat's internal self-shadow term is separate. |
| Groom as a Vulkan ray-query surface (#1253) | Approximate | `GroomSurfaceCache::Extract` converts and budgets resident strand proxy geometry for the GPU Scene. `GroomRayTracingProxyTest.cpp` checks tier/coverage/refusal contracts. Residency, update and triangle budgets can refuse an individual groom; a successful proxy is not per-strand RT. |

## Material, alpha and transmission

`SurfaceCategory` names the renderer family, `AuthoredKind` is the persisted
`MaterialKind` (`Generic`, `Snow`, `Skin`, `Foliage`), and `ClosureModel` selects
Legacy or ClosureV2 BRDF math. These are independent axes; neither material
kind nor closure version implies a particular alpha mode.

| Requests in `MaterialCoverageRows` | Outcome | Source and test evidence |
|---|---|---|
| Generic Legacy and Generic ClosureV2, opaque classic mesh | Supported | `MaterialKind.h` and `PBRModel.h` define independent on-disk choices. `MaterialLabVisualEvidenceTest.cpp` and `MaterialReferenceAovParityTest.cpp` exercise material lighting; the deferred G-buffer carries the model and kind flags. |
| Skin ClosureV2, opaque skinned mesh | Supported | `Renderer3DMeshSubmission.cpp` copies `MaterialKind::Skin` and skin profile fields into material data. `SkinTransmissionEvidenceTest.cpp` covers Forward, Forward+ and Deferred under changing illumination. This row itself requests opaque shading. |
| Foliage kind with thin transmission | Supported | `FoliageSurface.glsl` provides the thin two-sided lobe; `FoliageLeafTransmissionEvidenceTest.cpp::BacklitLeavesTransmitShadowedOnesDoNotAndEveryPathAgrees` checks the three GL paths. This does not represent volumetric refraction. |
| Water material on water geometry; wrong material on water or groom geometry | Supported; wrong material Unsupported | `Water` and `Fibre` are distinct renderer families, so the registry rejects category mismatches before selecting a technique. This is an admission contract, not an assertion that a changed material becomes water or hair. |
| Classic masked mesh requesting raster shadows | Approximate | `Renderer3DMeshSubmission.cpp` suppresses alpha-masked proxy shadow casting, and `SubmeshMaterialPathParityTest.cpp::AnAlphaMaskedSubmeshCastsNoSolidShadowOnAnyPath` checks the missing solid-quad shadow. Visible alpha cutout and shadow fidelity are separate outcomes. |
| Classic blended mesh on Deferred | Supported when the forward overlay is present | `Renderer3DMeshSubmission.cpp::ShouldRerouteToForwardOverlay` sends blends out of the opaque G-buffer. `DeferredForwardOverlayRouteTest.cpp::BlendedMaterialIsReroutedOnDeferred` pins selection; `TransparentBlendOrderVisualEvidenceTest.cpp` covers draw order. `Evaluate` does not check overlay availability, so runtime engagement must confirm it. |
| Classic refractive transmission on Forward or Deferred | Approximate | `TransmissionVisualEvidenceTest.cpp` covers Forward absorption and Deferred's forward-overlay reroute. The support contract deliberately uses `TransmissionApproximation` rather than implying physical equivalence across techniques. In particular, `Renderer3DMeshSubmission.cpp::DrawAnimatedMeshParallel` keeps a deferred transmissive skinned draw opaque and counts that fallback; serial `DrawAnimatedMesh` reroutes it. |

## Backend, path, samples and upscale

`PathCoverageRows` covers all six native raster backend/path pairs for a generic
opaque static mesh. `Supported` means the static request is admitted; this
table does not substitute for a live pixel capture on each backend.

| Requests in `PathCoverageRows` | Outcome | Source and test evidence |
|---|---|---|
| OpenGL and Vulkan × Forward, Forward+, Deferred, native, single sample | Supported static compatibility | `RenderingPath.h` defines the three paths and the renderer has backend-specific RHI execution. Material and path parity still require the task's separate artifact/live grid. |
| OpenGL Deferred, four samples, native or spatial upscale | Supported static compatibility | `VirtualGeometryVisualEvidenceTest.cpp::RendersUnderDeferredMSAA` covers the virtual geometry draw. `FSR2VisualEvidenceTest.cpp::MSAAFallsBackToSpatialAndStillRenders` checks the non-native spatial fallback. |
| OpenGL Deferred, one sample, temporal upscale, available temporal capability | Supported static compatibility | `TemporalUpscalePolicy.h::ShouldRunTemporalUpscale`, `FSR2PolicyTest.cpp` and `FSR2VisualEvidenceTest.cpp` cover the gate and rendered GL path. Runtime capability, render extent, and history still decide actual engagement. |
| OpenGL Deferred, four samples, temporal upscale | Unsupported | `TemporalUpscalePolicy.h` treats MSAA as a hard guard; `FSR2PolicyTest.cpp::MSAAIsAHardGuardNotADegradation` pins it. Spatial fallback is a different request. |
| Vulkan Deferred, temporal upscale | Unsupported | The FSR2 temporal backend is GL only; `FSR2PolicyTest.cpp::TemporalRunsOnlyWhenModeTechniqueAndBackendAllAgree` covers the policy. |
| OpenGL Deferred ray query; Vulkan Forward ray query; Vulkan Deferred without ray-query capability | Unsupported | `RendererSupport::Evaluate` reports `BackendHasNoRayQueries`, `RequiresDeferred`, and `RayQueryUnavailable`, respectively. Raster fallback is a separate request and must be reported as selected. |

The software virtual rasterizer implements triangle coverage rejects. The
Vulkan mesh-shader hardware route does not call `gl_CullPrimitiveEXT` or the
shared `VirtualRasterCoverage.glsl` helpers (#1049); that optimization is not
a separate supported rendering family.

## Contract boundaries

- A raster scene-shadow request for groom returns
  `Unsupported/GroomSceneShadowMissing`; a capable Vulkan ray-query request
  returns `Approximate/GroomRayProxy`. These are separate scene techniques.
- `RendererSupport::Evaluate` reports static compatibility. It cannot infer
  whether a particular frame produced or consumed the requested geometry or
  lighting. Pair its decision with the registry's requested/capable/selected/
  produced/consumed engagement data and live evidence before claiming a
  production preset passed.
