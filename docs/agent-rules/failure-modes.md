# agent-rules — by failure mode

Use this index when you know what you are doing but not yet what can go wrong. The archetypes cut across subsystems, because the failure transfers even when the code does not. The index by subsystem is [README.md](README.md).

## 1. The suite is green and the feature is broken

The dominant archetype: a passing run is not evidence, so name the observation that would have failed (a moving target, an edge, a second camera angle, or a physical quantity in pinned units).

| Doc | What stayed green |
|---|---|
| [groom-scene-lighting-and-wetness.md](groom-scene-lighting-and-wetness.md) | A coat in a closed red room stayed lit by open sky its body could not see. |
| [asset-loads-belong-to-the-game-thread.md](asset-loads-belong-to-the-game-thread.md) | Build Game worked on GL and lost the Vulkan device. |
| [scene-render-settings.md](scene-render-settings.md) | A game shipped through Build Game ran Forward whatever the scene was authored on. |
| [terrain-voxel-authored-state.md](terrain-voxel-authored-state.md) | A greedy terrain that lost its authored voxel volume on undo, Play or reload re-seeded a plausible copy. |
| [headless-input-window.md](headless-input-window.md) | A server ran its scene, then dereferenced its absent window on cursor release during shutdown. |
| [scene-registry-source-path.md](scene-registry-source-path.md) | Direct scene opens worked, but Build Game could not load a registered scene because its project-relative path was opened from the editor working directory. |
| [project-optional-paths.md](project-optional-paths.md) | An empty script-module path became the asset directory. |
| [force-model-vehicles.md](force-model-vehicles.md) | A boat with no thrust still floats and an oscillating aircraft still has finite positions. |
| [shadow-receiver-bias-is-texels-of-the-map-sampled.md](shadow-receiver-bias-is-texels-of-the-map-sampled.md) | A VSM-against-CSM floor test compared a shadow the VSM's own bias does not change. |
| [vulkan-software-driver-ci.md](vulkan-software-driver-ci.md) | A CI job on a driver below the ADR 0010 contract skipped every device-gated test and printed `[  PASSED  ]`. |
| [lavapipe-ray-query-suites.md](lavapipe-ray-query-suites.md) | Eight `ReSTIRPTDevice` cases "timed out on slow software compute". |
| [vulkan-support-matrix.md](vulkan-support-matrix.md) | A green `Windows.yml` while no runner could execute the Vulkan device or L7 ray-query suites. |
| [generated-evidence-stays-out-of-git.md](generated-evidence-stays-out-of-git.md) | Every PR stayed green while evidence commits grew the tracked tree to 2.19 GB. |
| [renderer-pr-gpu-tests-run-after-merge.md](renderer-pr-gpu-tests-run-after-merge.md) | A GTAO fix merged green because every GPU test skips on PR CI. |
| [jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md) | Unit tests pass whether the cape detaches, jitters or freezes rigid. |
| [foliage-hierarchical-wind.md](../guides/foliage-hierarchical-wind.md) | Reusing the current camera-facing card basis for previous positions loses wind-induced orientation motion even with a stationary camera. |
| [foliage-interaction-bending.md](../guides/foliage-interaction-bending.md) | A per-frame lerp recovery converges at a different speed on every machine and rings across a frame spike. |
| [foliage-interaction-bending.md](../guides/foliage-interaction-bending.md) | An influence that follows an actor makes the grass snap upright the instant it passes. |
| [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md) | Three separate bugs each rendered a plausible frame that read as "impostors missing". |
| [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md) | A pass that decides near-mesh versus far-card for itself renders a plant whose shadow is a different shape. |
| [vegetation-asset-import.md](vegetation-asset-import.md) | A foliage layer whose albedo shows a different species reads as four separate renderer bugs and is none of them. |
| [vegetation-asset-import.md](vegetation-asset-import.md) | Alpha coverage averaged over a texture sheet reported 69.5% where the canopy reads 54.4%. |
| [vegetation-asset-import.md](vegetation-asset-import.md) | A flipped glTF/OBJ v baked brown pine cones onto every foliage card, a plausible dead tree that passes every geometric check. |
| [foliage-lod-transition-coverage.md](foliage-lod-transition-coverage.md) | Decorrelating a LOD threshold cannot reduce flips per frame because crossings per metre are conserved. |
| [foliage-lod-transition-coverage.md](foliage-lod-transition-coverage.md) | A thinning fade band placed below the keep threshold thins the layer early, an error of 0.04% at full density that no screenshot shows. |
| [procedural-scatter-hash-and-habitat-rules.md](procedural-scatter-hash-and-habitat-rules.md) | A scatter hash whose two jitter draws differ by a constant gave 32 distinct offsets over 6400 cells and a screenshot that looks random. |
| [capture-paths-take-the-projection-seam.md](capture-paths-take-the-projection-seam.md) | An off-screen bake with a raw GL-convention ortho clipped every triangle on Vulkan and baked "successfully" as its clear colour, with no counter or validation error. |
| [screen-uv-unpack-takes-the-row-flip.md](screen-uv-unpack-takes-the-row-flip.md) | GTAO reconstructed every Vulkan pixel mirrored about the horizontal, hidden by a symmetric horizon seed until a 45-degree floor read 0.216 on Vulkan and 0.996 on GL. |
| [cpu-texel-rects-take-the-row-flip.md](cpu-texel-rects-take-the-row-flip.md) | The shadow pass copied the mirror image of a coat's rows on Vulkan. |
| [mesh-cache-must-return-the-surface-it-was-given.md](mesh-cache-must-return-the-surface-it-was-given.md) | A horse's coat stood at its bind pose while it walked, but only in the editor. |
| [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md) | A bright material rendered near-black and the test asserted nothing about it. |
| [visual-quality-criteria.md](visual-quality-criteria.md) | Three correct presence measurements marked "convincing geometry" met on a frame a human called 1998. |
| [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) | Settings reset on Play, and headless tests never call `Scene::Copy()`. |
| [renderer-state-machine-harness.md](renderer-state-machine-harness.md) | Four renderer bugs each lived in a sequence of valid operations. |
| [world-anchored-renderer-state-in-tests.md](world-anchored-renderer-state-in-tests.md) | A visual golden passes in file order and fails in a shard. |
| [ab-diff-peak-is-not-a-location.md](ab-diff-peak-is-not-a-location.md) | An A/B visual assertion read backwards in one test ordering, from frames that were identical to the byte. |
| [oit-blend-state-and-identity-lanes.md](oit-blend-state-and-identity-lanes.md) | A transparent decal drew and its fragments survived while the composite was unchanged. |
| [every-specular-lobe-takes-the-pixel-filter.md](every-specular-lobe-takes-the-pixel-filter.md) | The dog's eye bloomed white in one still in six, reading up to 845 where it should read about one. |
| [a-bound-coat-rides-its-body.md](a-bound-coat-rides-its-body.md) | The walking dog rendered right while its coat's shadow views cast 99% of it instead of 75%. |
| [groom-runtime-scalp-clearance.md](groom-runtime-scalp-clearance.md) | Correct scalp roots and static coverage hid guide arcs sagging into the head and across the face at runtime. |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | Two lighting bugs survived 4300 green tests. |
| [lighting-signal-contract.md](lighting-signal-contract.md) | DDGI, lightmaps, SSGI and ReSTIR GI lit surfaces pi times too bright or counted light twice, each a plausible frame because each estimator was right about its own output. |
| [completed-frame-perf-windows.md](completed-frame-perf-windows.md) | An estimated sleep labelled 120 history samples as a new camera window after only 66 new frames. |
| [measurement-validity-and-sentinels.md](measurement-validity-and-sentinels.md) | Four unrelated failures all published `0.0 ms`. |
| [renderer-memory-accounting.md](renderer-memory-accounting.md) | The Vulkan memory report read 6 MiB because no Vulkan code booked anything. |
| [binary-format-versioning.md](binary-format-versioning.md) | The save game carried 38 versions and 47 per-field gates because parallel branches collided on the version number and `AtEnd()` probes could not tell older from truncated. |
| [vulkan-query-generations.md](vulkan-query-generations.md) | Dense Vulkan frames produced backward whole-frame timestamps and plausible stale pass timings because query availability described the previous reset generation. |
| [planar-reflection-ao-restoration.md](planar-reflection-ao-restoration.md) | Reflection replay left white AO/depth fallbacks bound after the suspension flag cleared. |
| [engine-owned-containers.md](engine-owned-containers.md) | A `TArray` of a type that cannot survive bitwise relocation corrupts the heap on libstdc++ and is invisible on this box. |
| [shader-tuning-constants-need-a-unit.md](shader-tuning-constants-need-a-unit.md) | Every directional shadow sat metres away from its caster while the tests pinned the constant's unchanged value. |
| [shader-tuning-constants-need-a-unit.md](shader-tuning-constants-need-a-unit.md) | A crate seven metres under a spot cast no shadow. |
| [framebuffer-bind-owns-the-viewport.md](framebuffer-bind-owns-the-viewport.md) | Vulkan's `Bind()` kept the previous pass's viewport. |
| [cross-path-term-measurement-traps.md](cross-path-term-measurement-traps.md) | Every "direct light is invariant under GTAO" check passed while GTAO never ran. |
| [render-graph-repopulate-forces-setup.md](render-graph-repopulate-forces-setup.md) | A cached graph ran on a freshly re-populated blackboard for one frame per upscale toggle. |
| [gl-shader-route.md](gl-shader-route.md) | The dog showcase missed 60 fps on GL by 2-3.5 ms because NVIDIA ran the same GLSL several times slower from glShaderBinary SPIR-V than from text. |
| [glsl-shaders.md](glsl-shaders.md) §1a | A new `#extension` was green on every PR check and red on the next nightly. |
| [glsl-shaders.md](glsl-shaders.md) §1 | A padding lane called `_padding0` in a shared UBO block fails SPIR-V as a Vulkan-only error that every OpenGL test is blind to. |
| [amd-mesa-shader-compile-blowup.md](amd-mesa-shader-compile-blowup.md) | A shader compiled in a second on NVIDIA and llvmpipe. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | A corrupt drive mode clamped to a different valid mode, and the car still drove. |
| [asset-degradation-and-constructor-preconditions.md](asset-degradation-and-constructor-preconditions.md) | "Load the scene, does it crash?" passes because the trigger is resolution, not loading. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | An extension already in `s_ExtensionMap` routes the file to the OLD importer. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | A serializer that calls `AssetManager::GetAsset` from inside its own `TryLoadData` froze the editor with no crash, assert or log line. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | The MCP viewport override is in pixels but lands in a DPI-scaled field. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | A panel ranked into the `GraphCanvas` migration by line count has no pan, zoom, grid or wires at all. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | A zoom-scaled pin hit radius and a screen-space context-menu position both survived in three hand-rolled canvases because nothing tests viewport maths. |
| [notes-core-and-threading.md](notes-core-and-threading.md) | `constinit` on a `std::vector` is a hard compile error in any Debug build here, breaking the whole tree far from the code that reads it. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | When two serializers cover one type, a field only round-trips if you tested the one you did NOT write, as with `MaterialAsset`'s YAML loading typed maps into the generic map. |
| [asset-pack-round-trips-through-the-builder.md](asset-pack-round-trips-through-the-builder.md) | The packed-path test passed through the serializers while every pack the builder wrote named the wrong bytes. |
| [alpha-cutout-mip-chains.md](alpha-cutout-mip-chains.md) | A box mip chain keeps a cutout's coverage at cutoff 0.5 and loses it at every other cutoff. |
| [vulkan-block-compressed-upload.md](vulkan-block-compressed-upload.md) | Every cooked texture was null on Vulkan and the scene still rendered. |
| [notes-renderer.md](notes-renderer.md) | `Texture2D::Create(path, ...)` never returns null. |
| [notes-renderer.md](notes-renderer.md) | A two-sided surface lit from behind self-shadows to black because the cascade's receiver normal-offset walks the sample behind the surface's own depth. |
| [notes-renderer.md](notes-renderer.md) | A pass needing one more per-pixel scalar can use G-Buffer RT5's rgb. |
| [notes-renderer.md](notes-renderer.md) | Two uniform buffers on one binding point: the shader reads the one constructed last. |
| [notes-renderer.md](notes-renderer.md) | A world-space 2D component wired into only some of Scene's four `Renderer2D::BeginScene` sites silently vanishes in one viewport mode, usually the editor's 3D mode. |
| [notes-renderer.md](notes-renderer.md) | `Texture2D::SetData` does not flip rows as the image loader does. |
| [notes-renderer.md](notes-renderer.md) | Several passes estimating one quantity must composite bottom-up by confidence. |
| [notes-renderer.md](notes-renderer.md) | Perfectly repairing an approximation's two named error terms moved the image further from ground truth. |
| [notes-renderer.md](notes-renderer.md) | The three error terms were coupled, so repairing either side alone regressed and repairing both was a 3x win. |
| [notes-renderer.md](notes-renderer.md) | A tripwire test naming a fixed enum version stayed green through the change that superseded it. |
| [notes-renderer.md](notes-renderer.md) | A cumulative version list copied to nine sites went stale three times, always at the editor-inspector copy that no evidence test can reach. |
| [notes-renderer.md](notes-renderer.md) | A system that regenerates its instances wholesale renumbers every plant on each regeneration and nothing notices. |
| [notes-renderer.md](notes-renderer.md) | `TerrainData::GetHeightAt` / `GetNormalAt` each call `SyncFromGPU()`. |
| [notes-renderer.md](notes-renderer.md) | A terrain rebuild marked the splatmap and collision dirty but not the foliage on the same entity, leaving every plant at its old height. |
| [notes-renderer.md](notes-renderer.md) | A field added to seven of the eight shaders declaring `PBRMaterialProperties` relayouts the eighth's heap offsets. |
| [notes-renderer.md](notes-renderer.md) | The G-Buffer flags lane's closure-model field can only grow below it, halving the exact-representation ceiling per bit. |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | A test believed it exercised the manual path while a valid navmesh had switched it to the crowd follower. |
| [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) | A steady-state offset check passes with a full one-tick lag present. |
| [parallelizable-mover-systems.md](parallelizable-mover-systems.md) | A position check passes on the scheduler tie-break alone, with the dependency edge missing. |
| [multi-animal-scheduling-budgets.md](multi-animal-scheduling-budgets.md) | Forty animals each concluded they deserved full rate. |
| [mcp-protocol-eras.md](mcp-protocol-eras.md) | Adding `server/discover` alone keeps tests green and breaks the legacy fallback for real clients. |
| [automation-event-bus.md](automation-event-bus.md) | The push stream served every event unredacted while `olo_events_tail` served it scrubbed. |
| [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md) | `tools/list` serves an exposure profile, not the registry. |
| [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md) | A command that never declares `AutomationUndo` is silently refused from every transaction. |
| [delayed-read-test-must-prove-the-new-version.md](delayed-read-test-must-prove-the-new-version.md) | Two of five delayed-read tests passed with their producer reverted to in-place reuse. |
| [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) | Two scenes rendered skybox-only with zero errors because no test interleaved two SSBO uploads with draws. |
| [discard-is-not-a-bounds-check.md](discard-is-not-a-bounds-check.md) | Every software-raster resolve index was guarded by a `discard` and every CPU-side value read back valid. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A new `RHI::Access` write member is classified as a read by the one switch with a `default:`. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A BLAS build with `firstVertex = BaseVertex` over already-global indices applies the offset twice and device-loses on the submesh that runs past the buffer. |
| [vulkan-async-compute-queue.md](vulkan-async-compute-queue.md) | A queue-family ownership transfer naming its two families on only one half leaves contents undefined on AMD. |
| [vulkan-extension-adoption-claims.md](vulkan-extension-adoption-claims.md) | An issue's claim that a new extension removes a create-time constraint was built on. |
| [vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md) | A device fault reports a bare address, and correlating it with an engine destroy log by handle finds nothing because the validation layer wraps non-dispatchable handles. |
| [shared-uniform-binding-bind-before-dispatch.md](shared-uniform-binding-bind-before-dispatch.md) | The vegetation deformer wrote its params with `SetData` and never bound them, working until an animated fox's deformer bound its own block at the shared slot and the shader device-faulted. |
| [gpuav-rewrites-the-usage-it-judges-you-against.md](gpuav-rewrites-the-usage-it-judges-you-against.md) | GPU-AV adds `STORAGE_BUFFER_BIT` to application buffers. |
| [vulkan-shader-heap-indexing.md](vulkan-shader-heap-indexing.md) | Heap-indexing shaders fail on built-in names written from memory, misaligned slot regions, combined samplers passed to functions, and a material heap offset memoised across a texture reload. |
| [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md) | Every Vulkan draw wrote colour attachment 0 alone, and the forward path only displays attachment 0. |
| [a-pass-opens-its-own-colour-mask.md](a-pass-opens-its-own-colour-mask.md) | A debug line ending the G-Buffer bucket left attachments 1-4 masked in the editor only. |
| [substituted-seams-compound.md](substituted-seams-compound.md) | A decal tenant made three substitutions, each hiding a different live bug. |
| [testing-architecture.md](testing-architecture.md) §10 | A skipped Vulkan suite used to report exactly like a passing one. |
| [no-silent-fallbacks.md](no-silent-fallbacks.md) | Two defensible fallbacks composed into VK_ERROR_DEVICE_LOST on every virtual-geometry scene, three layers from the cause. |
| [multi-mode-block-format-encoders.md](multi-mode-block-format-encoders.md) | A misplaced field in one of fourteen block layouts corrupts only the blocks that chose that mode. |
| [compute-written-texture-mip-chain.md](compute-written-texture-mip-chain.md) | A compute kernel wrote mip 0 and left coarser mips stale. |
| [gpu-scan-compaction.md](gpu-scan-compaction.md) | A compaction test that sorts both sides passes identically on `atomicAdd` and on the scan replacing it. |
| [variable-rate-compute-shading.md](variable-rate-compute-shading.md) | A classifier that coarsens nothing passes every "did coarsening damage the image" assertion. |
| [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md) | Every glyph was invisible on AMD with 852 quads submitted. |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | A merged quad with U and V swapped still merges and draws. |
| [cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md) | Green on every CI run because a runner never has a warm cache. |
| [observer-camera.md](observer-camera.md) | The frozen cut looks plausible when culling quietly follows the observer. |
| [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md) §14, §16, §18 | Every tenant and sweep was green while the first full virtual-geometry frame on Vulkan failed three ways. |
| [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md) | A missing VNDF weight renders believable, permanently too-bright specular. |
| [screen-space-denoiser-chain.md](screen-space-denoiser-chain.md) | A half-resolution filter guided by the full-resolution G-Buffer rejects every silhouette tap. |
| [temporal-reactivity-separation.md](temporal-reactivity-separation.md) | A strand whose coverage changed passes every history test so the old value is dragged across the LOD step. |
| [temporal-coverage-means-not-samples.md](temporal-coverage-means-not-samples.md) | TAA's coverage range test fired on 1.8% of a still stochastic coat's pixels every frame yet saw only 15% of the coat when every strand halved in width. |
| [animation-clock-survives-unrendered-ticks.md](animation-clock-survives-unrendered-ticks.md) | A previous-time sentinel reset on each unrendered tick zeroed wind, water and foliage velocity on every frame of a throttled editor, and four trace rounds missed the writer. |
| [temporal-long-sequence-protocol.md](temporal-long-sequence-protocol.md) | Colour histories outside the registry missed camera-cut resets and each test's first frames blended the previous test's image, while every settled screenshot looked correct. |
| [temporal-measurement-control-arms.md](temporal-measurement-control-arms.md) | Three control arms in a row measured nothing and every headline number looked excellent, including a "feature off" arm that shimmered exactly 0. |
| [pixel-error-mesh-lod.md](pixel-error-mesh-lod.md) | Projecting through the real view-projection instead of a facing plane passes every value test. |
| [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md) | A vertical wall at the tile edge and six flat-coloured islands. |
| [water-shading-nyquist.md](water-shading-nyquist.md) | Normals derived without the amplitude the displacement carried went unnoticed for months. |
| [geometry-lod-measure-the-unreachable-cost.md](geometry-lod-measure-the-unreachable-cost.md) | A tess-control cull rejecting 95% of patches still pays every vertex invocation. |
| [persistent-world-space-fields.md](persistent-world-space-fields.md) | A wake in an R8 texture renders and follows correctly and never fades. |
| [compute-in-place-vs-ping-pong.md](compute-in-place-vs-ping-pong.md) | Adding a neighbour read to an in-place compute pass races between work groups and renders in bands that follow the dispatch order. |
| [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md) | Shader and CPU agreed on the function and disagreed on which space its argument was in. |
| [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) §3 | A guard added to the one reader with no callers left the live path reading the previous-pose palette raw. |
| [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) | The three shadow-path skinning copies lost the bone-ID bounds test and zero-weight guard, collapsing an unweighted vertex onto the origin in shadows only. |
| [morph-and-lod-in-the-animated-surface.md](morph-and-lod-in-the-animated-surface.md) | A morphing face ghosted under TAA because a moved surface has no expressible velocity. |
| [per-draw-material-stamp-rides-the-copy.md](per-draw-material-stamp-rides-the-copy.md) | The expression-driven pore band was inert on every path because `Material`'s hand-written copy constructor dropped the field, and the only test hand-stamped the component's material. |
| [creature-eye-region.md](creature-eye-region.md) | Every eye test passed while the showcase dog's eyes sat in bald domes inside craters with reversed blink signs. |
| [groom-coat-colour.md](groom-coat-colour.md) | The golden's coat read its colour map upside down for its whole life. |

## 2. One contract, several mirrors

The same fact is written in more than one place with nothing enforcing agreement, so use a parity test that reads both sides as text or make one side derived, because a "keep in sync" comment is not a mechanism.

| Doc | The mirrors |
|---|---|
| [distance-impostor-reflection-probes.md](distance-impostor-reflection-probes.md) | Header, bake and GLSL, pinned by a regex parity test. |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | Three light evaluators that can render the same scene. |
| [directional-shadow-ownership.md](directional-shadow-ownership.md) | Which light owns the directional cascades is decided in `Scene.cpp` by EnTT iteration order and re-decided by type alone in ten lit shaders. |
| [lighting-signal-contract.md](lighting-signal-contract.md) | The ambient ladder, one `E -> E/pi` conversion and the composition every raster path calls, each now written once where there had been copies. |
| [ddgi-probe-cascades-and-sparsity.md](ddgi-probe-cascades-and-sparsity.md) | The cascade-shift invalidation is derived independently on CPU and GPU, on purpose. |
| [render-graph-declaration-config.md](render-graph-declaration-config.md) | Every declaration input was written where it was read and again in a hand-assembled cache fingerprint. |
| [reference-path-tracer.md](reference-path-tracer.md) | A C++ BRDF port against the GLSL it mirrors. |
| [resampled-estimator-measure-convention.md](resampled-estimator-measure-convention.md) | A missing change-of-measure Jacobian or a double-applied normaliser survives a golden, a Jacobian unit test and CPU/GPU agreement, because both implementations shared the misunderstanding. |
| [gpu-path-tracer.md](gpu-path-tracer.md) | A GLSL path tracer against the C++ one it mirrors, pinned on the device by a written-down disagreement budget. |
| [baked-lightmap-pipeline.md](baked-lightmap-pipeline.md) | The GI stores' units ledger, and bake-time unwrap parameters mirrored by the runtime resolve. |
| [lightmap-receiver-identity.md](lightmap-receiver-identity.md) | Four walks that must gather the same lightmap receivers, or it renders with no baked GI and no error. |
| [lightmap-receiver-identity.md §VirtualGeometry](lightmap-receiver-identity-details.md#virtualgeometry-the-uv2-rides-the-vertex-arena-because-no-binding-was-available) | A reserved vertex-pull binding resolves from the VAO. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | Five exclusion sets, each mirrored by a coverage-test roster. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | The build pipeline and the runtime must agree on an asset layout. |
| [audio-voice-budget.md](audio-voice-budget.md) | One config field costs four edits, one of them silent. |
| [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) | Two build trees writing the same generated files. |
| [worktree-shared-build-outputs.md](worktree-shared-build-outputs.md) | A second build tree in the same worktree deleted glad's generated header mid-compile and overwrote engine archives the first tree then linked without rebuilding. |
| [windows-codeview-links.md](windows-codeview-links.md) | A process-count limit leaves lld worker pools unbounded. |
| [build-memory-per-tu.md](build-memory-per-tu.md) | Five sources for a build-memory number disagreed by 3x while `--parallel` and two cgroup caps rested on it. |
| [build-trees-and-windows-asan.md §4b](build-trees-and-windows-asan-details.md#4b-live-toolchain-bug-a-throw-from-inside-a-catch-handler-avs-clang-cl--asan) | A throw executed inside a `catch` handler AVs under clang-cl ASan, whatever the catch type and rethrow form. |
| [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md) | A static destructor read a `thread_local` that `__dyn_tls_dtor` had already destroyed, giving 219 failures from one bug. |
| [concurrent-cmake-configure.md](concurrent-cmake-configure.md) | Two configures sharing one `CMakeFiles/`, where the nested `try_compile` that loses reports the error. |
| [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) | Four subsystems hold world-space state outside the rebased set. |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | The packed-quad layout lives in `VoxelQuad.h` and `VoxelQuadUnpack.glsl`, and a mismatch compiles. |
| [destructible-debris.md](destructible-debris.md) | Two unrelated physics layer numberings, so `SetCollisionLayer(Debris)` never reaches Jolt's `DEBRIS`. |
| [structural-collapse.md](structural-collapse.md) | A `Static` body cannot become `Dynamic` unless it was created with `mAllowDynamicOrKinematic`. |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) | Four uint packings in C++ and four GLSL files, where a wrong bit renders plausible wrong content. |
| [gpu-scene-record-contract.md](gpu-scene-record-contract.md) | Five records in `GPUSceneTypes.h` and `include/GPUScene.glsl`, pinned by `static_assert` and a SPIRV-Cross reflection test. |
| [gpu-scene-record-contract.md §7](gpu-scene-record-contract-details.md#7-the-raster-consumer-a-draw-link-resolved-once-per-frame) | Every unmigrated raster path keeps its own previous-frame transform, pinned by `GPUSceneAntiDuplicationRatchetTest`. |
| [animated-surface-records.md](animated-surface-records.md) | One record carries two histories with different derivation rules. |
| `ShaderBindingLayout.h` <-> `include/BindlessHeap.glsl` | `HEAP_IMAGE_SLOT_BASE` is derived in C++ and a literal in GLSL, so adding any `TEX_*` slot is also a shader edit (#702). |
| [ssbo-binding-cap-is-80-on-mesa.md](ssbo-binding-cap-is-80-on-mesa.md) | The SSBO ceiling lived in prose copied from the UBO namespace while the driver's real 80 was mirrored nowhere. |
| [groom-coat-colour.md](groom-coat-colour.md) | The UV convention a groom source writes root UVs in against the one the engine samples textures in, pinned by `TheRootUVsAreTheSkinsUnderThem`. |

## 3. It silently drops something

No crash, no error, no log line: work or data disappears and the system keeps running, so ask what the absence would look like, and if nothing would differ you need a coverage test, not a unit test.

| Doc | What was dropped |
|---|---|
| [raw-bindless-route-driver-front-end.md](raw-bindless-route-driver-front-end.md) | The bindless route itself, per program: NVIDIA rejected a parameter named `packed` and a 15th fragment uniform block, both programs fell back and rendered, and every SPIR-V check stayed green. |
| [ci-oom-read-the-kernel-report.md](ci-oom-read-the-kernel-report.md) | The heavy-TU compile bound, so the AMD nightly was OOM-killed for 20 nights. |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | The neighbour rebuild after a carve, leaving a stale wall. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | A field, from every save. |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) §4 | An unmapping, once publishing became incremental. |
| [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) | A settings struct, on entering Play. |
| [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md) §7 | A virtual-shadow page request once the ring filled. |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | An authored light parameter, turned into a dead knob. |
| [reference-path-tracer.md](reference-path-tracer.md) §5 | DDGI's whole infinite-bounce term for any probe volume fitted to a room. |
| [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) | World position, gradually. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | Every script, in shipped games only. |
| [packaged-game-content-by-path.md](packaged-game-content-by-path.md) | Every authored plant and impostor in a shipped game, because no `.obj` was ever packaged. |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | A component's teardown, when `m_Registry.destroy()` skips `OnComponentRemoved`. |
| [vcpkg-dependency-management.md](vcpkg-dependency-management.md) | Three of five traps are silent, including a port option that never applied and one that switched on and killed TSan. |
| [asset-import-usd-alembic.md](asset-import-usd-alembic.md) | Winding, up-axis, unit scale, UV origin. |
| [openusd-build-and-link.md](openusd-build-and-link.md) | OpenVDB's oneTBB, which ran on USD's older bundled copy because USD's archive came first on the link line with `/FORCE:MULTIPLE` on. |
| [asset-import-openvdb-volumetric.md](asset-import-openvdb-volumetric.md) | A fog volume rendered as a solid box through every non-VDB-aware evaluator. |
| [groom-curve-import.md](groom-curve-import.md) | Authored groom intent when an unknown attribute is ignored rather than refused. |
| [groom-strand-visibility.md](groom-strand-visibility.md) | A whole coat to an alpha cutoff on sub-pixel strands. |
| [velocity-convention.md](velocity-convention.md) | A stochastic coat kept 3x of its 11x temporal accumulation because every writer put the jitter delta into every velocity. |
| [groom-scene-temporal-resolve.md](groom-scene-temporal-resolve.md) | Every coat in an authored scene, to a stochastic mode refused for want of a TAA box nobody ticked. |
| [groom-surface-binding.md](groom-surface-binding.md) | A coat onto the wrong part of a body after a silent rebind. |
| [groom-fibre-scattering.md](groom-fibre-scattering.md) | A coat's whole azimuthal response to a quadrature that point-samples the fibre width. |
| [groom-guide-simulation.md](groom-guide-simulation.md) | A coat that lags its animal by a whole animation, to a simulated guide the deformer never selected, among seven other solver failures. |
| [groom-guide-stand-ins.md](groom-guide-stand-ins.md) | A long coat 14% less shadowed at 14 m, because stand-ins averaged away the part of each guide's drape pointing its own way. |
| [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md) | A coat's whole shadow, to a reference footprint narrower than the strand spacing that reports a confident zero. |
| [groom-dual-scattering.md](groom-dual-scattering.md) | A golden coat rendered grey, flat and plush, to fibre crossings treated as opaque and a uniform-sky TT term lighting strands from behind the body. |
| [groom-dual-scattering.md](groom-dual-scattering.md) | A white coat read grey in the shade because Zinke's back-scatter series was cut at three scatters and counted where no coat was behind the strand. |
| [groom-coat-rest-bake.md](groom-coat-rest-bake.md) | A dense bound coat spent 74 ms posing segments and 123 ms baking its self-shadow on the CPU on every frame it breathed. |
| [groom-coat-body-in-the-volume.md](groom-coat-body-in-the-volume.md) | The light the body should stop, because the self-shadow volume held only strands. |
| [groom-strand-depth-prepass.md](groom-strand-depth-prepass.md) | A close-up coat shaded tens of times per pixel because the coverage discard kept early depth rejection from culling strands behind. |
| [groom-deformed-coat-self-shadowing.md](groom-deformed-coat-self-shadowing.md) | A walking animal carrying its bind-pose shadow. |
| [groom-representation-lod.md](groom-representation-lod.md) | Four fifths of a coat's density to a borrowed foliage square-root compensation. |
| [groom-lod-cross-fade-and-detail-bias.md](groom-lod-cross-fade-and-detail-bias.md) | A shadow check for a fading coat passed with the caster mask removed. |
| [rt-scene-follows-the-raster-caster-set.md](rt-scene-follows-the-raster-caster-set.md) | Every ray-traced shadow in the dog scene, because a lawn that casts no shadow queued 385 MiB anyway and the fail-closed budget withheld the whole TLAS. |
| [rt-vegetation-budget-charged-where-enforced.md](rt-vegetation-budget-charged-where-enforced.md) | Every ray-traced pass with RT reflections on, to a lawn whose builds the backend dropped every frame past a cap only the backend charged. |
| [rt-quality-pressure-plans-a-complete-tier.md](rt-quality-pressure-plans-a-complete-tier.md) | Every ray-traced effect, to one casting vegetation group that did not fit and caused both TLAS addresses to be withheld. |
| [groom-ray-traced-proxies.md](groom-ray-traced-proxies.md) | A coat to black, to a groom made a ray-traced-shadow receiver as well as a caster, plus four other proxy failures. |
| [groom-card-coverage.md](groom-card-coverage.md) | A card tier drawing 2.6x its coat, to card widths summed over members that overlap, among nine other coverage failures. |
| [groom-coat-bake-cost.md](groom-coat-bake-cost.md) | 95 ms a frame for three walking coats' self-shadow. |
| [groom-gpu-strand-deformation.md](groom-gpu-strand-deformation.md) | A herd's frame, to every bound strand rebuilt and re-uploaded on the CPU each frame. |
| [coat-authoring-and-per-role-budgets.md](coat-authoring-and-per-role-budgets.md) | An animal's silhouette, to one global stride that thins sparse guard hairs and dense undercoat by the same fraction. |
| [mcp-protocol-eras.md](mcp-protocol-eras.md) | Event pushes, when the notification carrier was swapped instead of run beside the old one. |
| [steamworks-platform-integration.md](steamworks-platform-integration.md) | The whole feature, when the SDK path is one level too high. |
| [configure-time-variable-visibility.md](configure-time-variable-visibility.md) | A DLL copy step for the test executable, on the first configure only. |
| [cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md) | A texture, from the second load onward. |
| [shared-atlas-allocator.md](shared-atlas-allocator.md) | A budget claim, when `vector::resize()` or a `= T{}` reset discards a non-RAII handle. |
| [technique-selection-seams.md](technique-selection-seams.md) | The reason a light did not get the technique it asked for, when the choice is a shader branch and nothing on the CPU made the decision. |
| [derived-graph-must-outlive-its-source.md](derived-graph-must-outlive-its-source.md) | Every scene from the asset dependency graph, because no serializer registered one and every registered edge pointed backwards. |

## 4. Your instrument is lying to you

The check passes for a correct implementation and for a broken one, so measure the noise floor first and construct a case the instrument must fail on before trusting a case it passes.

| Doc | The instrument that failed |
|---|---|
| [temporal-coverage-means-not-samples.md](temporal-coverage-means-not-samples.md) | A "two draws of the same frames" stability ratio improved from 0.66 to 0.50 when TAA stopped clipping the coat. |
| [gl-write-once-buffers-are-immutable-storage.md](gl-write-once-buffers-are-immutable-storage.md) | The renderer memory report showed the groom rest stream's 1.5 GB of VRAM while the GL driver kept a second resident copy the report could not see. |
| [claude-code-mods.md](claude-code-mods.md) | A build piped into `tail` reported exit code 0 after ninja stopped. |
| [measure-in-the-frame-the-pixels-came-from.md](measure-in-the-frame-the-pixels-came-from.md) | The walking dog's regions unprojected 60 frames through the first frame's camera. |
| [retained-scratch-has-an-owner-and-a-ledger.md](retained-scratch-has-an-owner-and-a-ledger.md) | The groom memory report looked complete while seven function-local `thread_local` scratch vectors held the largest coat's working set outside it. |
| [caster-bounds-come-from-the-geometry.md](caster-bounds-come-from-the-geometry.md) | The caster share's "lower bound" took a scaled sample and a padded box, claiming 146 of 137 and crediting 19.8 NDC of 0.0002. |
| [editor-input-coordinates-and-imgui-viewports.md](editor-input-coordinates-and-imgui-viewports.md) | `olo_input_inject` refused the right-hand dock as outside a 1280x720 window, a size cached before `GLFW_SCALE_TO_MONITOR` resized it to 1920x1080. |
| [live-verification-noise-floor.md](live-verification-noise-floor.md) | A crop check scored a mirrored, wrong position better. |
| [verification-workload-attribution.md](verification-workload-attribution.md) | Debug CPU overhead improved in Release, but the dense scene still spent hundreds of milliseconds in GPU shadows. |
| [forward-deferred-parity-measurement.md](forward-deferred-parity-measurement.md) | A live Forward-vs-Deferred diff blamed the point-light evaluator for a gap that was screen-space AO. |
| [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md) | A GPU counter that stopped updating is byte-identical to one that is constant. |
| [vulkan-parallel-cascade-recording-fault.md](vulkan-parallel-cascade-recording-fault.md) | A device-fault checkpoint named `ScenePrepassPass` in every report. |
| [motion-claims-need-runtime-frames.md](motion-claims-need-runtime-frames.md) | The dog's shimmer test "in motion" read back editor frames, where the guide solver never steps. |
| [automation-build-invocation.md](automation-build-invocation.md) | A build's exit code is 0 three ways without anything having been built: the lock's stand-down, a no-op incremental. |
| [incomplete-texture-samples-as-zero.md](incomplete-texture-samples-as-zero.md) | A sampled zero is a value, not an error, so the frame is wrong exactly where the feature is active, on one vendor only. |
| [water-displaced-once-and-pinned-captures.md](water-displaced-once-and-pinned-captures.md) | A live-clock editor capture compared two wave phases and read as a backend geometry difference. |
| [std-distributions-are-not-portable.md](std-distributions-are-not-portable.md) | Two platforms disagree about procedural content, or a test passes on one and fails on the other with no GPU difference behind it. |
| [procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md) | A red that recurs every run gets normalised and blinds the suite. |
| [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md) | A `glXxx(` grep is wrong three different ways. |
| [volumetric-cloud-debugging.md](volumetric-cloud-debugging.md) | Capture targets show the editor camera, include-only shader edits do not hot-reload. |
| [timed-wait-test-assertions.md](timed-wait-test-assertions.md) | `duration_cast<milliseconds>` truncates toward zero. |
| [shared-temp-dir-test-isolation.md](shared-temp-dir-test-isolation.md) | A CI comment asserted a safety property nobody had measured. |
| [cross-test-renderer-state.md](cross-test-renderer-state.md) | A visual test passed 10/10 in isolation and drew nothing in the full run. |
| [test-runtime-camera-defaults-to-orthographic.md](test-runtime-camera-defaults-to-orthographic.md) | A groom culled by a default orthographic camera kept the last capture's stats. |
| [reference-path-tracer.md](reference-path-tracer.md) | A golden answers "did it change", never "is it correct". |
| [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md) | A per-vendor baseline validates itself, and a small cross-vendor RMSE measures portability, not correctness. |
| [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md) | Every obvious noise metric passes on white noise. |
| [gpu-debug-draws.md](gpu-debug-draws.md) | Read the two-counter overflow protocol before concluding "it drew nothing". |
| [static-archive-4gib-ceiling.md](static-archive-4gib-ceiling.md) | `LNK1248` reports the offset where the archive crossed 4 GiB, not its size. |
| [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) §5e, §6 | A CI job that said `--config Release` built no config. |
| [lazy-static-release-ownership.md](lazy-static-release-ownership.md) | GL has no allocator-teardown assertion, so a clean GL close proves nothing about teardown. |
| [incremental-build-odr-staleness.md](incremental-build-odr-staleness.md) | A correct fix that a live rebuild kept "disproving", because the binary was stale, not the source. |
| [pch-masked-missing-includes.md](pch-masked-missing-includes.md) | A green Windows build says nothing about a header's self-containment. |
| [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) | A cache fails by being slow, not by looking wrong: every save in the repo was refused for six days and every run stayed green. |
| [actions-cache-budget.md](actions-cache-budget.md) | The Windows sccache entry had not existed for weeks. |
| [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md) | A healthy restore still hit 32%, and `--show-stats` printed the same size line for a cache 20% over its cap and one with spare room. |
| [windows-sccache-warm-reuse.md](windows-sccache-warm-reuse.md) | The nightly restored a same-commit, same-toolchain snapshot and still missed on all 2060 requests while ASan hit 99.90%. |
| [self-hosted-ccache-slot-multiplier.md](self-hosted-ccache-slot-multiplier.md) | A cumulative 76.40% hit rate over a cache pinned at its cap averaged the warm era with the thrash. |
| [cache-entry-version-is-the-path-string.md](cache-entry-version-is-the-path-string.md) | The vcpkg key matched exactly and the entry was still invisible. |
| [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md) | A shader-only change took compile time on a warm cache. |

## 5. Ordering and lifetime

The logic is right, but when it runs or how long it lives is wrong.

| Doc | The ordering or lifetime rule |
|---|---|
| [script-structural-command-safe-point.md](script-structural-command-safe-point.md) | Never mutate the registry mid-iteration. |
| [visual-script-vm.md](visual-script-vm.md) | A loop node that forgets to charge its own iteration hangs the frame. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | Five ordering rules for a swap that destroys the thing being iterated. |
| [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) | Input before the physics kick, camera last. |
| [fixture-assets-outlive-the-case.md](fixture-assets-outlive-the-case.md) | The dog's cases left about 120 MB of registered assets each and crossed the 6 GB ceiling 24 cases in. |
| [cancel-a-load-by-removing-its-record.md](cancel-a-load-by-removing-its-record.md) | A cancelled load's result is dropped by removing its record. |
| [two-phase-occlusion-culling.md](two-phase-occlusion-culling.md) | Pass order decides who still sees previous-frame depth. |
| [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) | Clearing the LRU bit one step early evicts the whole cache every frame. |
| [ccache-pch-result-key-collision.md](ccache-pch-result-key-collision.md) | Two runner slots write their `.pch` to one ccache result key. |
| [groom-into-the-shadow-techniques.md](groom-into-the-shadow-techniques.md) | A whole coat casts nothing while every counter reads healthy because its strands are a thousandth of a shadow texel wide, among six other caster and receiver ordering failures. |
| [groom-casters-in-vsm-local-layers.md](groom-casters-in-vsm-local-layers.md) | Every lamp the VSM served cast no coat shadow while the sun's did. |
| [virtual-geometry-into-a-second-shadow-technique.md](virtual-geometry-into-a-second-shadow-technique.md) | A whole caster family loses its shadow when a second technique is switched on and nobody routed it there. |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) | Touch a priority-ordered LRU in reverse (§5), and fill coarse-to-fine with one dispatch per level with barriers (§3a). |
| [async-fault-in-under-a-residency-cache.md](async-fault-in-under-a-residency-cache.md) | Allocating the slot before the asynchronous read completes evicts a resident entry to hold nothing. |
| [render-pass-published-state.md](render-pass-published-state.md) | Publish last, restore deliberately. |
| [render-graph-out-of-band-work.md](render-graph-out-of-band-work.md) | Edges that existed only as registration order, `DependsOnPass` strings or `NeverCull` made the occlusion pyramid read correct only because of `AddNode` order. |
| [vulkan-descriptor-heap-rebind-after-execute-commands.md](vulkan-descriptor-heap-rebind-after-execute-commands.md) | A primary's heap binding is undefined after `vkCmdExecuteCommands`. |
| [transparent-draw-order.md](transparent-draw-order.md) | An inverted depth in the least significant field ordered blended draws only within one shader+material bucket and composited the rest by shader ID then material ID. |
| [registries-must-outlive-their-registrants.md](registries-must-outlive-their-registrants.md) | A lazily-created registry is destroyed before the namespace-scope statics whose destructors unregister from it. |
| [cluster-lod-simplification.md](cluster-lod-simplification.md) | A lock must outlive the level that created it (§1). |
| [deforming-geometry-conservative-bounds.md](deforming-geometry-conservative-bounds.md) | A rest-pose bound stops containing deforming geometry so the cull drops on-screen clusters. |
| [render-graph-attachment-feedback.md](render-graph-attachment-feedback.md) | A renamed framebuffer hid a shader read of the attachment it was writing. |
| [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md) | A read from a pooled resource whose lifetime already ended. |
| [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md) | TOCTOU between a decrement and a re-read. |
| [non-recursive-lock-self-locking-helper.md](non-recursive-lock-self-locking-helper.md) | A locked scope calling a sibling that locks the same non-recursive mutex. |
| [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md) | Promoting a per-tick local to persistent state. |
| [bump-allocator-rollover-padding.md](bump-allocator-rollover-padding.md) | A bump allocator sized its new block with padding computed for the old base, then recomputed padding with no final bounds check, and the cursor advanced out of the block. |
| [parallelizable-mover-systems.md](parallelizable-mover-systems.md) | Split a system at its write boundary. |
| [gl-clear-program-revalidation.md](gl-clear-program-revalidation.md) | What is bound when you clear. |
| [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) | A CPU write between two recorded draws is last-write-wins on Vulkan. |
| [vulkan-large-vertex-stream-command-ordered.md](vulkan-large-vertex-stream-command-ordered.md) | Every animated coat on Vulkan failed on a per-frame snapshot 8-14x the size of the arena slot it had to fit in. |
| [vulkan-sampler-array-bindings.md](vulkan-sampler-array-bindings.md) | A sampler array resolved element `i` by heap-slot adjacency. |
| [vulkan-parallel-recording.md](vulkan-parallel-recording.md) | Two parallel items wrote one UBO object or transitioned one subresource. |
| [command-packet-lifecycle.md](command-packet-lifecycle.md) | A pass edited the packets of a bucket it had already replayed. |
| [vulkan-parallel-graph-recording.md](vulkan-parallel-graph-recording.md) | Prepared passes never form a production group, or disabled members make a valid group decline. |
| [vulkan-parallel-pass-audit.md](vulkan-parallel-pass-audit.md) | A pass that prepared for parallel recording published frontend state before the join, or counted preparation as proof that it ran in parallel. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A per-instance BLAS loop builds one structure twice. |
| [deformed-surfaces-in-acceleration-structures.md](deformed-surfaces-in-acceleration-structures.md) | An animated mesh whose records became stageable is built into a rest-pose BLAS and traced T-posed with healthy counters. |
| [foliage-regeneration-is-async-once-workers-start.md](foliage-regeneration-is-async-once-workers-start.md) | Seven foliage evidence tests passed alone and failed after another suite had started the task workers. |
| [gpu-scan-compaction.md](gpu-scan-compaction.md) | A `barrier()` only some invocations reach. |
| [lazy-static-release-ownership.md](lazy-static-release-ownership.md) | A shared lazy static released from a conditional teardown. |
| [configure-time-variable-visibility.md](configure-time-variable-visibility.md) | A CMake variable read by a subdirectory processed before the line that sets it. |

## 6. It was never actually called

Code that exists and is tested but has no production caller, or has more callers than the grep shows.

| Doc | The finding |
|---|---|
| [server-authoritative-networking-loop.md](server-authoritative-networking-loop.md) | Twenty tested networking classes produced nothing because the drive function had zero call sites. |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | `AddAgent` / `SetAgentTarget` had zero production callers. |
| [audio-voice-budget.md](audio-voice-budget.md) | Sounds start from six call sites, not the two a grep suggests. |
| [substituted-seams-compound.md](substituted-seams-compound.md) | Both decal paths drew zero fragments for as long as they existed. |
| [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md) §7 | The game scene never set `TessellationEnabled` and no shipped scene set `UseImpostor`. |
| [render-pass-published-state.md](render-pass-published-state.md) | `MeshComponent { Primitive: 0 }` is `None`: an entity that never renders. |
| [foliage-vsm-page-caching.md](foliage-vsm-page-caching.md) | Foliage submitted to the VSM as deforming every frame redrew every layer into all sixteen clip levels (19-103 ms at 1080p) and the page cache never applied to it. |
| [shadow-pass-attribution-and-cull-slots.md](shadow-pass-attribution-and-cull-slots.md) | The lamps' shadow atlas cost 0.1-0.9 s with no bracket naming it. |
| [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) §5 | A `Setup()` that early-returns while a feature is off freezes that decision into the frame-graph fingerprint. |
| [render-graph-setup-declaration-gates.md](render-graph-setup-declaration-gates.md) | One frame rendered with the bucket empty culled a pass that declares nothing without work. |
