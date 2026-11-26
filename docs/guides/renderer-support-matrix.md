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
| Virtual skinned mesh, ray query or path tracing (#1525) | Unsupported | `Renderer3DMeshSubmission.cpp` refuses to stage its fixed rest-pose proxy for a skinned part; `ProxylessParts` and `GPUSceneUnsupportedCategory::Virtualized` count the refusal. |
| Deforming classic skinned mesh, ray query, deformed vertex stream missing | Unsupported | `RayTracingScene::Classify` refuses the animated instance and increments `AnimatedInstancesRefused`; tracing its rest pose would produce wrong shadows. |
| Deforming classic skinned mesh, ray query, deformed vertex stream ready | Supported | `RayTracingScene::Classify` admits the per-instance deformed stream for refit. The support request must supply that stream observation; device ray-query support alone is insufficient. |
| Virtual morph target | Unsupported | `VirtualMeshBuilder::BuildSet` rejects sources with morph targets (`VirtualMeshBuilder.cpp`). `Request::MorphTargets` records this independently of skinning, material and transmission. |
| Virtual WPO (#1152), tessellation/displacement (#1153), spline/landscape (#1154), programmable raster (#1155) | Unsupported | The virtual-geometry builder, registry and virtual raster shaders have no implementation for these families. Classic terrain and water tessellation are separate representations and do not satisfy a virtual-geometry request. |
| Gaussian splat as a production scene entity (#1046) | Unsupported | `Renderer/Splat` has PLY decode, ordering and LOD code, and `GaussianSplatVisualEvidenceTest.cpp` renders an isolated GL fixture. There is no splat scene component, asset route or render-pipeline pass. |
| Groom raster strands without scene shadow | Supported | `GroomRenderPass` draws strands and `GroomStrand.glsl` samples the coat's internal optical-depth volume. This row makes no scene-shadow claim. |
| Groom raster scene-shadow casting or receiving (#1323, restored by #1523) | Supported | `ShadowRenderPass` casts grooms into the CSM cascades, the local-light atlas, the Virtual Shadow Map's clip levels and its local-light layers (#1592), deforming a bound coat in the depth shaders; `GroomStrand.glsl` samples the scene's shadow at the coat's light-exit point. `GroomSceneShadowVisualEvidenceTest.cpp` pins both directions on GL Forward, Forward+ and Deferred. |
| Groom as a Vulkan ray-query surface (#1253) | Approximate | `GroomSurfaceCache::Extract` converts and budgets resident strand proxy geometry for the GPU Scene. `GroomRayTracingProxyTest.cpp` checks tier/coverage/refusal contracts. Residency, update and triangle budgets can refuse an individual groom; a successful proxy is not per-strand RT. |

## Raster shadow caster coverage

VSM replaces directional CSM while enabled; `VSM.LocalLights` additionally
replaces the point/spot atlas. The current family routes are independent of the
Forward, Forward+ or Deferred receiver path.

| Caster family | CSM and local atlas | VSM directional clips | VSM point/spot layers |
|---|---|---|---|
| Static and skinned mesh | Yes | Yes | Yes |
| Tessellated or ordinary terrain | Yes | Yes | Yes |
| Marching-cubes and greedy cubic voxel geometry | Yes | Yes | Yes |
| Foliage cards, authored meshes and impostors | Yes | Yes | Yes |
| Virtual geometry | Yes | Yes | Unsupported |
| Groom ribbons | Yes | Yes | Yes (#1592) |

Terrain, voxel and foliage VSM depth wrappers use the same deformation, alpha
discard and LOD bodies as their CSM/atlas shaders. Shadow extraction includes
offscreen terrain chunks, with shadow-view culling. Cached footprints cover
creation, movement, height/geometry changes and removal; moving foliage dirties
its conservative footprint. Pending GPU height edits use unknown bounds and
conservative invalidation until the CPU mirror is synchronized.

`ShadowFamilyCache` CPU tests pin identity, invalidation and removal.
`ShadowFamilyVisualEvidence` measures isolated-family contribution and world
position on all three GL paths, with local-layer, presentation and lifecycle
controls. The full retained presentation matrix is selected with
`OLO_VSM_FAMILY_FULL_MATRIX=1`; shader compilation alone is not Vulkan visual
evidence. See [the VSM page-cache guide](../agent-rules/virtual-shadow-map-page-cache-details.md#7-caster-coverage)
for the resource and live-verification contract.

VSM remains off by default. `OLO_VSM_STARTUP=0|1` and
`OLO_VSM_LOCAL_LIGHTS_STARTUP=0|1` seed the existing shadow settings during
initialization, allowing cooked runtime A/B verification without a new scene
schema. Later settings writes remain authoritative. Startup logs report both
requested and effective VSM state. `olo_terrain_voxel_load` imports existing
VOX1 data into an enabled editor terrain, with undo. Authored voxel content is
persisted by scene YAML (and so by asset packs) and by save games; an
auto-seeded greedy volume is not, and re-seeds from the height field.

## Material, alpha and transmission

`SurfaceCategory` names the renderer family, and `AuthoredKind` carries the persisted
`MaterialKind` (Generic, Snow, Skin or Foliage). Standard PBR materials use ClosureV2.
Alpha mode and thin transmission remain independent material inputs.

| Requests in `MaterialCoverageRows` | Outcome | Source and test evidence |
|---|---|---|
| Generic opaque classic mesh | Supported | `MaterialLabVisualEvidenceTest.cpp` measures the roughness and metallic sweep. `MaterialReferenceAovParityTest.cpp` checks material lighting. The deferred G-buffer carries the kind and profile slot. |
| Skin ClosureV2, opaque skinned mesh | Supported | `Renderer3DMeshSubmission.cpp` copies `MaterialKind::Skin` and skin profile fields into material data. `SkinTransmissionEvidenceTest.cpp` covers Forward, Forward+ and Deferred under changing illumination. This row itself requests opaque shading. |
| Foliage kind with thin transmission | Supported | `FoliageSurface.glsl` provides the thin two-sided lobe; `FoliageLeafTransmissionEvidenceTest.cpp::BacklitLeavesTransmitShadowedOnesDoNotAndEveryPathAgrees` checks the three GL paths. This does not represent volumetric refraction. |
| Water material on water geometry; wrong material on water or groom geometry | Supported; wrong material Unsupported | `Water` and `Fibre` are distinct renderer families, so the registry rejects category mismatches before selecting a technique. This is an admission contract, not an assertion that a changed material becomes water or hair. |
| Classic masked mesh requesting raster shadows | Approximate | `Renderer3DMeshSubmission.cpp` suppresses alpha-masked proxy shadow casting, and `SubmeshMaterialPathParityTest.cpp::AnAlphaMaskedSubmeshCastsNoSolidShadowOnAnyPath` checks the missing solid-quad shadow. Visible alpha cutout and shadow fidelity are separate outcomes. |
| Classic blended mesh on Deferred | Supported when the forward overlay is present | `Renderer3DMeshSubmission.cpp::ShouldRerouteToForwardOverlay` sends blends out of the opaque G-buffer. `DeferredForwardOverlayRouteTest.cpp::BlendedMaterialIsReroutedOnDeferred` pins selection; `TransparentBlendOrderVisualEvidenceTest.cpp` covers draw order. `Evaluate` does not check overlay availability, so runtime engagement must confirm it. |
| Classic refractive transmission on Forward or Deferred | Approximate | `TransmissionVisualEvidenceTest.cpp` covers Forward absorption and Deferred's forward-overlay reroute. The support contract deliberately uses `TransmissionApproximation` rather than implying physical equivalence across techniques. A deferred transmissive skinned draw is rerouted by `DrawAnimatedMesh`. |

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

## Scene views

A second scene view (`Renderer3D::CreateView`, `Scene::RequestAdditionalView`) renders in the same
physical frame as the main view with its own render graph, histories, exposure, occlusion pyramid,
light grid and shadow maps ([ADR 0026](../adr/0026-a-scene-view-is-one-struct-and-a-frame-is-one-step-of-the-world.md)).
`Supported` here means each view renders the frame a single-view renderer renders from that camera,
texel for texel, in both orders. Cost:
[multi-view-cost-1352.md](../analysis/multi-view-cost-1352.md).

| Two views in one frame | Outcome | Source and test evidence |
|---|---|---|
| OpenGL and Vulkan × Forward, Forward+, Deferred, different sizes and poses | Supported | `MultiViewRenderingTest.cpp::EachOfTwoViewsRendersWhatASingleViewRendersFromItsCamera` runs in both test processes. |
| One view temporally resolved and spatially upscaled, the other native | Supported | `MultiViewRenderingTest.cpp::ATemporallyResolvedViewAndANativeViewKeepTheirOwnHistories`: jitter indices and histories are per view. |
| A different rendering path and sample count per view | Supported | `MultiViewRenderingTest.cpp::PathAndSampleCountDifferPerView` (Deferred 4x beside Forward+, and Forward beside Deferred 4x). |
| Eye adaptation per view | Supported | `MultiViewRenderingTest.cpp::EachViewAdaptsItsExposureToWhatItSees`: each view steps by the time since it last rendered. |
| Resize, camera cut or destruction of one view | Supported | `MultiViewRenderingTest.cpp::ResizingCuttingOrDestroyingOneViewLeavesTheOtherIntact` (the second view is the one changed) and `MultiViewRenderingTest.cpp::ResizingOrCuttingTheMainViewLeavesTheSecondIntact` (the main view is): the other view's histories, pyramid and shadow maps keep their identities and ages. |
| An object moving between frames, motion blur in both views | Supported | `MultiViewRenderingTest.cpp::AMovingObjectBlursTheSameInBothViews`: object poses rotate to "previous" once per physical frame. |
| Foliage (card grass with wind, density LOD and distance fade) in both views, OpenGL and Vulkan | Supported | `MultiViewFoliage.EachViewDrawsTheMeadowASingleViewDrawsFromItsCamera` compares exact frames. Placement is published once per frame. Culling, thinning and fading are per view. Stable GPU compaction preserves canonical draw order under TAA (#1695). |
| Foliage streamed mesh detail seen from a later view | Approximate | Residency is decided once per frame from the primary view's camera; a later view looking elsewhere draws the stand-ins that camera left there. |
| A world content change (streaming tier switch, foliage regeneration) | Supported | `MultiViewRenderingTest.cpp::AWorldInvalidationReachesEveryViewAndAViewsOwnOnlyItself`: `Renderer3D::InvalidateWorldTemporalHistories` drops every view's histories. |
| A bound, simulated coat (groom) in both views | Supported | `GroomSimulationVisualEvidenceTest.cpp::ASecondViewOfTheFrameLeavesTheCoatTheMainViewDraws` (OpenGL): the binding's history and the simulation's displacement pair advance on the coat's first publish of the frame, so every view draws the same coat with the same previous frame. |
| Planar reflection in both views | Supported | `MultiViewRenderingTest.cpp::EachViewReflectsFromItsOwnCamera`: each view mirrors its own camera. |
| Ray-traced effects in both views, Vulkan with a ray-tracing device | Supported | `MultiViewRenderingTest.cpp::BothViewsTraceTheFramesAccelerationStructure`: one acceleration-structure build per frame, traced by every view. |
| Views whose cameras need different camera-relative origins | Approximate | Every view of a frame renders about the primary view's origin; a view far from it has the float precision of that distance. |
| Camera-following simulation (wind field, snow clipmap, wake and foam lattice, spray, precipitation) seen from a later view | Approximate | Centred on and stepped by the frame's primary view; a later view sees those fields where its frustum overlaps them. |
| More views in one Vulkan frame than the transient descriptor ring holds views for (1024 transient views per frame, shared) | Approximate | The views past the limit bind the slot-based way; the heap counts them (`Stats::TransientOverflowsHeld`) and reports once per session. `RHIDescriptorHeapTest.cpp::AnOverflowUnderAHoldIsCountedAsTheFramesViewLimit`. OpenGL has no such limit. |
| Two `BeginScene` calls without `Renderer3D::BeginFrame` | Unsupported as one frame | Each is a physical frame: the world advances twice, as before views existed. |
| Views of two scenes in one bracket | Unsupported | The second scene's view is refused its GPU Scene draw links and the renderer says so once. |
| An editor viewport rendered through a second view | Unsupported | Nothing in the editor creates a view. |
| A reflection or light probe bake through a view other than the main one | Unsupported | The bakers activate the main view whichever view their caller had active (`MultiViewRenderingTest.cpp::ABakeStartedUnderAnotherViewRendersThroughTheMainView`, OpenGL): a view made for a bake would capture unconverged per-view accumulations (DDGI cascades, the virtual shadow cache). |

Asset thumbnails (`AssetPreviewRenderer`) and the sky and IBL cubemap bakes are not scene views: they
draw with their own shaders and framebuffers and never go through `Renderer3D`'s views, so a second
view changes nothing for them.

## Contract boundaries

- A raster scene-shadow request for groom returns `Supported`; a capable
  Vulkan ray-query request returns `Approximate/GroomRayProxy`. These are
  separate scene techniques.
- `RendererSupport::Evaluate` reports static compatibility. It cannot infer
  whether a particular frame produced or consumed the requested geometry or
  lighting. Pair its decision with the registry's requested/capable/selected/
  produced/consumed engagement data and live evidence before claiming a
  production preset passed.
