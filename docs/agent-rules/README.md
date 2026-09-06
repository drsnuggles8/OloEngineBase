# agent-rules — the index

Two kinds of document live here:

- **Postmortems**: one real failure each, written so the next person doesn't repeat it.
- **Reference guides**: the `notes-*.md` family, accumulated per-subsystem gotchas.

Use **Part A** when you know what you are touching. Use **Part B** when you know what you are
doing but not yet what can go wrong; the archetypes there cut across subsystems, and the failure
transfers even when the code does not.

Each entry is one sentence stating the rule. The story that taught it is inside the file.

---

# Part A: by subsystem

## Code and review standards

- [cpp-coding-quality.md](cpp-coding-quality.md): the coding rules, including float comparison, `auto`, IWYU, and the defaulted `operator==` MSVC quirk.
- [glsl-shaders.md](glsl-shaders.md): the SPIR-V rules a shader must follow to compile: no bare uniforms, UBO bindings, MRT outputs, and never `.length()` on a storage buffer (§6b — Vulkan rejects it at pipeline creation, long after the SPIR-V validated).
- [technique-selection-seams.md](technique-selection-seams.md): when a subsystem grows a second way of computing the same number, the choice is a value carrying the reason it is not what was asked for — not another shader `if`, and not a new row in a path enum.
- [sonarqube-review-alignment.md](sonarqube-review-alignment.md): read before `/code-review` so local findings match the cloud profile.

## Testing and verification

- [testing-architecture.md](testing-architecture.md): which renderer layer or Functional axis a new test belongs to, and the registration contract.
- [../testing.md](../testing.md): why we test what we test; value heuristic, anti-patterns, retirement criteria.
- [substituted-seams-compound.md](substituted-seams-compound.md): every substitution a test makes is a seam it stops testing, and they compound — including building the same object a different way.
- [no-silent-fallbacks.md](no-silent-fallbacks.md): a path that cannot do what it was asked says so loudly and countably; rank a fallback by whether the substituted value can be INDEXED, and lower every entry point a caller falls back to.
- [reference-path-tracer.md](reference-path-tracer.md): the ground-truth oracle for "is it correct", where a golden can only say "did it change".
- [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md): measure the noise floor and audit a recording before baking a per-vendor baseline.
- [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md): give a visual-test scene a ground plane, then look at the PNG.
- [live-verification-noise-floor.md](live-verification-noise-floor.md): measure frame-to-frame noise before attributing a pixel change, and confirm the editor is drawing at all.
- [procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md): a generator fix and its golden rebake ship in the same PR.
- [timed-wait-test-assertions.md](timed-wait-test-assertions.md): measure timed waits in microseconds and assert one-sided.
- [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md): keep the "is it still alive?" signal in a trivially destructible `thread_local`; a destroyed one is not readable.
- [shared-temp-dir-test-isolation.md](shared-temp-dir-test-isolation.md): use `TestTempDir.h`, never a fixed temp path; every test case is its own process.
- [cross-test-renderer-state.md](cross-test-renderer-state.md): never shut down a process-wide singleton you did not start, and leave the renderer configuration as you found it; plus the two traps that make single-process bisection lie.
- [world-anchored-renderer-state-in-tests.md](world-anchored-renderer-state-in-tests.md): a fixture that builds a `Scene` per test must also reset the process-static renderer state scene load resets.

## Build and dependencies

- [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md): never build msvc and clangcl trees together; caches, link bounds, memory, the local ASan recipe.
- [concurrent-cmake-configure.md](concurrent-cmake-configure.md): one configure at a time per build tree; the error blames your CMakeLists.txt and LTO instead.
- [static-archive-4gib-ceiling.md](static-archive-4gib-ceiling.md): a .lib cannot exceed 4 GiB, and `LNK1248` under-reports the overshoot.
- [vcpkg-dependency-management.md](vcpkg-dependency-management.md): read before adding, bumping or removing a dependency; the CRT triplet mismatch is heap corruption.
- [configure-time-variable-visibility.md](configure-time-variable-visibility.md): a CMake variable must be set before the `add_subdirectory()` that reads it.
- [asset-import-usd-alembic.md](asset-import-usd-alembic.md): the importer registry seam, and vendoring OpenUSD / Alembic / MaterialX statically.
- [asset-import-openvdb-volumetric.md](asset-import-openvdb-volumetric.md): keep OpenVDB editor-only; derive the grid transform without hand-transposing; extend every exhaustive `switch`.
- [incremental-build-odr-staleness.md](incremental-build-odr-staleness.md): when a correct fix makes no sense live, suspect a stale incremental object before the code.
- [pch-masked-missing-includes.md](pch-masked-missing-includes.md): a header must include what it uses even when the build is green; the PCH hides the omission on Windows and only the Linux jobs name it.
- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md): a CI cache that restores is not
  one that works.
- [actions-cache-budget.md](actions-cache-budget.md): the Actions cache store is a fixed budget every key must fit in; past the cap it goes read-only and refuses every save in the repo, silently.
- [cache-entry-version-is-the-path-string.md](cache-entry-version-is-the-path-string.md): a cache's restore and its save must be handed the same path string, character for character; two spellings of one directory are two caches.
- [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md): print the cache statistics after every CI build and read the uncacheable line; a PCH without `CCACHE_SLOPPINESS` and a per-commit macro each made every engine object a miss.
- [shader-pack-bake.md](shader-pack-bake.md): the CI-baked `.osp` pack, its content-hash invalidation, and why a fresh worktree does not fetch it.
- [steamworks-platform-integration.md](steamworks-platform-integration.md): the SDK is developer-supplied, CI builds a stub, and exactly one TU may include a Valve header.

## Renderer

- [multi-mode-block-format-encoders.md](multi-mode-block-format-encoders.md): a block format with several block layouts is validated one mode at a time against the reference decoder's own read order; BC6H interpolates in half-float space, so a high-contrast block is not its hard case; a compute port keeps its integer half identical and returns bulk data in an `rgba32ui` image rather than spending a scarce buffer binding.
- [incomplete-texture-samples-as-zero.md](incomplete-texture-samples-as-zero.md): set a texture's sampler state at creation; an incomplete texture reads zero on AMD and fine on NVIDIA.
- [std-distributions-are-not-portable.md](std-distributions-are-not-portable.md): seed procedural content with your own transform over mt19937; std:: distributions differ between standard libraries, so one seed is two different results.
- [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md): the OpenGL boundary leaks through the include graph, not a `glXxx(` grep; plus the Vulkan epic's lessons.
- [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md): a CPU buffer write between two recorded draws is last-write-wins on Vulkan — and the per-draw snapshot that fixes it must never cover a buffer the GPU produces.
- [vulkan-parallel-recording.md](vulkan-parallel-recording.md): a pass forks with `RenderCommand::RecordParallel` and gives every item its own resource objects; per-command-buffer state is per recording context.
- [vulkan-parallel-graph-recording.md](vulkan-parallel-graph-recording.md): schedule ready prepared passes before compiling resource lifetimes, and publish shared state only after joining.
- [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md): a BLAS is per geometry and opacity is per instance; acceleration structures reach a shader as a device address, builds ride the frame command buffer, and compaction is a multi-frame handshake because idling is banned.
- [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md): `glColorMask` and `glEnable(GL_BLEND)` are indexed calls for every draw buffer; never port one as a fallback.
- [lazy-static-release-ownership.md](lazy-static-release-ownership.md): release a shared lazy static from an unconditional teardown, not from `Renderer3D::Shutdown`.
- [registries-must-outlive-their-registrants.md](registries-must-outlive-their-registrants.md): a process-wide registry a destructor unregisters from must be a deliberately leaked singleton, never a plain static.
- [gpu-debug-draws.md](gpu-debug-draws.md): any shader can draw a primitive into the viewport; read the overflow protocol before concluding "it drew nothing".
- [observer-camera.md](observer-camera.md): the frozen culling camera decides what is drawn, never how it looks.
- [gpu-scan-compaction.md](gpu-scan-compaction.md): no early return in front of a work-group scan; test compaction order, not sets.
- [variable-rate-compute-shading.md](variable-rate-compute-shading.md): measure departure from a plane, not depth range, and read the heatmap first.
- [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md): publish GPU counters by name without stalling; the buffer-binding namespace is full.
- [ssbo-binding-cap-is-80-on-mesa.md](ssbo-binding-cap-is-80-on-mesa.md): every `SSBO_*` binding stays below 80, because Mesa exposes 80 storage-buffer binding points and the UBO namespace's 84 does not bound them.
- [gpu-scene-record-contract.md](gpu-scene-record-contract.md): a GPU-scene record changes in C++ and GLSL in one commit, and only an incompatible edit or a removal advances its generation.
- [gpu-scene-record-contract.md §7](gpu-scene-record-contract.md#7-the-raster-consumer-a-draw-link-resolved-once-per-frame): a migrated draw carries a link to its record, resolved once after the commit, and every path that still duplicates transform truth is named in `GPUSceneLegacyAdapters.h`.
- [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md): blue noise is a claim about the error spectrum; the VNDF weight fails silently; a resolve clips, not clamps.
- [screen-space-denoiser-chain.md](screen-space-denoiser-chain.md): a filter that runs at half resolution is guided by a guide buffer at its own resolution, its geometry test is a plane distance, and only a stage's SIZE is graph topology.
- [gl-clear-program-revalidation.md](gl-clear-program-revalidation.md): wrap every new clear site in `GLClearProgramGuard`, unbind and restore.
- [render-pass-published-state.md](render-pass-published-state.md): a pass that publishes engine-global bindings runs last and is not wrapped in `GLStateGuard(Restore)`.
- [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md): `WriteNewVersion` renames a physical resource; use the poison and disable levers to find stale reads.
- [render-pipeline-caches.md](render-pipeline-caches.md): process-wide render caches invalidate on every topology reset, not only on a fingerprint change.
- [ddgi-probe-cascades-and-sparsity.md](ddgi-probe-cascades-and-sparsity.md): the DDGI clipmap is toroidal, `%` truncates toward zero, and sparsity fails as "no GI, no error".
- [baked-lightmap-pipeline.md](baked-lightmap-pipeline.md): the GI units ledger, UV2 as a parallel stream, and keying the seam split after the unwrap.
- [lightmap-receiver-identity.md](lightmap-receiver-identity.md): a baked lightmap region is addressed by (entity UUID, sub-key), never by the entity alone.
- [two-phase-occlusion-culling.md](two-phase-occlusion-culling.md): phase 1 tests the previous frame's final pyramid; pass order decides who sees old depth.
- [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md): four page-cache invariants; a `Setup()` that branches on a runtime toggle is frozen by the fingerprint cache.
- [cluster-lod-simplification.md](cluster-lod-simplification.md): a terminal group's boundary lock outlives the level that created it.
- [pixel-error-mesh-lod.md](pixel-error-mesh-lod.md): the LOD plane faces the mesh, not the camera, and the error metric must be a ratio.
- [compute-written-texture-mip-chain.md](compute-written-texture-mip-chain.md): every writer of mip 0 owes the rest of the chain.
- [terrain-gpu-lod-quadtree.md](terrain-gpu-lod-quadtree.md): crack-freedom is a vertex-set property; picking must not inherit the tessellation gate.
- [terrain-virtual-texturing.md](terrain-virtual-texturing.md): every VT defect is a wrong address; touch LRU in reverse priority; an eviction is an entry, not an absence.
- [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md): measure a tile's outermost ring, and never key an auto-material rock rule below the shoreline mask's slope.
- [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md): the packed-quad encoding is mirrored in GLSL, and a merged quad can render plausibly and wrong six ways.
- [camera-relative-rendering.md](camera-relative-rendering.md): every world-space GPU upload is a site; f32 cancellation shows as jitter and shadow swim.
- [distance-impostor-reflection-probes.md](distance-impostor-reflection-probes.md): one encoding contract mirrored in three places, and a miss sentinel that shades from stale sky.
- [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md): three ways impostor cards go missing, separable only by reading PNGs from several azimuths.
- [light-path-photometric-parity.md](light-path-photometric-parity.md): the three light evaluators must agree; a dropped GPU struct field is a dead knob.
- [volumetric-cloud-debugging.md](volumetric-cloud-debugging.md): eight causes of a uniform veil, and how to tell "darker" from "directionally darker".
- [water-shading-nyquist.md](water-shading-nyquist.md): a derived normal carries every factor its displacement carries; drop sub-pixel detail rather than filter it.
- [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md): the shared thing between a shader and gameplay sampling must be an analytic record, in one agreed space.
- [persistent-world-space-fields.md](persistent-world-space-fields.md): a multiplicative decay is unrepresentable in a normalized-integer texture below a rate threshold.
- [compute-in-place-vs-ping-pong.md](compute-in-place-vs-ping-pong.md): a compute pass may update a field in place only while every invocation reads its own texel.
- [pbf-solver-stability.md](pbf-solver-stability.md): PBF/SPH reference constants assume unit-mass particles.
- [shared-atlas-allocator.md](shared-atlas-allocator.md): the buddy allocator behind the shadow atlas and impostor budget; swap, don't mutate, and beware the non-RAII handle.

## Scene, ECS and serialization

- [component-serializer-codegen.md](component-serializer-codegen.md): when a component round-trips for free, when to annotate a field, and every generated touch-point's exclusion set.
- [scene-binary-sidecar.md](scene-binary-sidecar.md): the `.scenebin` fast path: generated, hybrid-covered, and how it is invalidated — reordering a covered component's fields is a version bump like adding one.
- [binary-format-versioning.md](binary-format-versioning.md): gate each new field of a fixed-order archive; the header check does not exclude old data.
- [cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md): a cache must refuse to store a name nothing can resolve; the failure shows on the second load only.
- [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md): `Scene::Copy()` must carry every scene-level settings struct into Play.
- [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md): four subsystems hold world-space state outside the rebased set, each needing a different fix.
- [asset-degradation-and-constructor-preconditions.md](asset-degradation-and-constructor-preconditions.md): a precondition asserted in a constructor delegates safety to every call site.

## Gameplay, physics and simulation

- [force-model-vehicles.md](force-model-vehicles.md): boats and aircraft driven by `AddForce`; every bug here leaves the suite green.
- [jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md): drive a pinned cloth vertex by velocity, never by position.
- [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md): a `CharacterVirtual` is invisible to UUID-keyed body filters, and a follow camera runs last.
- [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md): a valid navmesh silently switches every `NavAgentComponent` onto the crowd follower.
- [parallelizable-mover-systems.md](parallelizable-mover-systems.md): how to move an entity-moving system onto a worker thread, and the two traps in its determinism test.
- [terrain-collision-streaming-sculpt.md](terrain-collision-streaming-sculpt.md): streamed tile bodies live in a second `JoltScene` map, and tile pose must match the draw transform exactly.
- [procedural-skinned-mesh-primitives.md](procedural-skinned-mesh-primitives.md): code-built test meshes need a live GL context and a shared bind-pose origin.
- [destructible-debris.md](destructible-debris.md): pre-authored debris chunks, and two unrelated physics layer numberings.
- [audio-voice-budget.md](audio-voice-budget.md): admit the voice cap inside `Play()`, because sounds start from six call sites.

## Scripting, networking and tooling

- [script-structural-command-safe-point.md](script-structural-command-safe-point.md): a script binding that changes the registry structurally queues a command, never acts inline.
- [visual-script-vm.md](visual-script-vm.md): a loop node charges its own iteration, memoization is per exec step, and `PinType` numbering is on disk.
- [runtime-scene-switching.md](runtime-scene-switching.md): the host applies a scene swap after the tick; five ordering rules and the `Project` mount.
- [server-authoritative-networking-loop.md](server-authoritative-networking-loop.md): grep for callers of the entry point, not for tests.
- [mcp-setter-based-field-registry.md](mcp-setter-based-field-registry.md): copy-then-swap MCP writes are unsound when `operator=` cannot reproduce a setter's side effects.
- [mcp-protocol-eras.md](mcp-protocol-eras.md): the stateless core is a second transport; adding `server/discover` alone breaks working clients.

## Concurrency and memory

- [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md): a decrement-then-reread of a refcount is a double-free even with atomics.
- [non-recursive-lock-self-locking-helper.md](non-recursive-lock-self-locking-helper.md): fix the callee that locks internally; don't wrap a self-synchronised member in an outer lock.
- [spinlock-payload-cache-line-separation.md](spinlock-payload-cache-line-separation.md): keep a lock off its payload's cache line.
- [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md): three checks before promoting a per-tick scratch vector to persistent state.

## Subsystem notes (`notes-*.md`)

Accumulated per-subsystem gotchas. Skim the relevant one before working in that area.

- [notes-renderer.md](notes-renderer.md): offline capture, GL wrappers, shader bindings, SSAO/SSR/FSR, IBL bakes, GPU timers, which per-instance bounds list survives frustum culling, why two uniform buffers on one binding point is last-created-wins, why tightening a loop bound moves every guard that was calibrated against it, the four separate Scene draw sites a world-space 2D component must be wired into, and why a texture built in memory is vertically mirrored relative to one loaded from a file.
- [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md): the three-part tool split, schemas, consent and undo, frame capture.
- [notes-core-and-threading.md](notes-core-and-threading.md): yaml-cpp decode, `Ref<T>` constness, the task system, EnTT first-touch, C++ traps.
- [notes-gameplay-physics-nav.md](notes-gameplay-physics-nav.md): the two Jolt systems, joints, Detour, dialogue guards, the gameplay scheduler.
- [notes-audio-animation-sim.md](notes-audio-animation-sim.md): pose sampling, retargeting, morph targets, the fixed-timestep split, SoundGraph.
- [notes-editor-and-assets.md](notes-editor-and-assets.md): Content Browser, filewatch import, placeholders, texture cook, the ScriptCore build edge, why a second meaning for an already-registered extension loses silently, and why an asset serializer that resolves a second asset used to deadlock the whole editor with no diagnostic at all.

---

# Part B: by failure mode

## 1. The suite is green and the feature is broken

The dominant archetype here. If your change is in one of these areas, a passing run is not evidence.

| Doc | What stayed green |
|---|---|
| [force-model-vehicles.md](force-model-vehicles.md) | A boat with no thrust still floats and an oscillating aircraft still has finite positions. |
| [jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md) | Unit tests pass whether the cape detaches, jitters or freezes rigid. |
| [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md) | Three separate bugs each rendered a plausible frame that read as "impostors missing". |
| [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md) | A bright material rendered near-black and the test asserted nothing about it. |
| [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) | Settings reset on Play, and headless tests never call `Scene::Copy()`. |
| [world-anchored-renderer-state-in-tests.md](world-anchored-renderer-state-in-tests.md) | A visual golden passes in file order and fails in a shard, because it encoded the previous test's residue. |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | Two lighting bugs survived 4300 green tests. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | A corrupt drive mode clamped to a different valid mode, and the car still drove. |
| [asset-degradation-and-constructor-preconditions.md](asset-degradation-and-constructor-preconditions.md) | "Load the scene, does it crash?" passes because the trigger is resolution, not loading. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | An extension already in `s_ExtensionMap` routes the file to the OLD importer, so a new format fails as a corrupt version of the old one rather than as an unsupported format. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | A serializer that calls `AssetManager::GetAsset` from inside its own `TryLoadData` re-enters the importer's non-recursive registry mutex: the editor freezes mid-frame with no crash, no assert and no log line, looking exactly like a slow shader warmup. |
| [notes-renderer.md](notes-renderer.md) | Two uniform buffers on one binding point: the shader reads the one constructed LAST, so the other's SetData lands where nothing reads and the frame renders with a zeroed camera. |
| [notes-renderer.md](notes-renderer.md) | A new world-space 2D component wired into only some of Scene's four `Renderer2D::BeginScene` sites draws in one viewport mode and silently vanishes in another; the editor's 3D-mode site is the one usually missed, and it is the path headless visual-evidence tests exercise. |
| [notes-renderer.md](notes-renderer.md) | The image loader flips rows on load but `Texture2D::SetData` does not, so an atlas built in a test is vertically mirrored against the same image on disk and every sub-rect samples its neighbour; a "did anything draw" assertion cannot see it. |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | A test believed it exercised the manual path while a valid navmesh had switched it to the crowd follower. |
| [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) | A steady-state offset check passes with a full one-tick lag present. |
| [parallelizable-mover-systems.md](parallelizable-mover-systems.md) | A position check passes on the scheduler tie-break alone, with the dependency edge missing. |
| [mcp-protocol-eras.md](mcp-protocol-eras.md) | Adding `server/discover` alone keeps tests green and breaks the legacy fallback for real clients. |
| [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) | Two scenes rendered skybox-only with zero errors because no test interleaved two SSBO uploads with draws. Later, the same snapshot applied to a GPU-output buffer made the mesh-shader raster arm launch `EmitMeshTasksEXT(0)` — a legal call, so nothing warned — while masking an out-of-bounds read that device-faults once you remove it. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A new `RHI::Access` write member is classified as a read by the one switch with a `default:`, so no write-after-write barrier is emitted and nothing warns. |
| [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md) | Every Vulkan draw wrote colour attachment 0 alone, and the forward path only displays attachment 0. |
| [substituted-seams-compound.md](substituted-seams-compound.md) | A decal tenant made three substitutions, each hiding a different live bug; no decal had ever produced a pixel. |
| [no-silent-fallbacks.md](no-silent-fallbacks.md) | Two defensible fallbacks composed into VK_ERROR_DEVICE_LOST on every virtual-geometry scene, three layers from the cause, with both Vulkan VG tests passing. A stub returning `nullptr` then vanished into the caller's own "mapping failed" branch, so no page upload landed and every counter still read green. |
| [multi-mode-block-format-encoders.md](multi-mode-block-format-encoders.md) | A misplaced field in one of fourteen block layouts corrupts only the blocks that chose that mode, and the aggregate PSNR barely moves. |
| [compute-written-texture-mip-chain.md](compute-written-texture-mip-chain.md) | A compute kernel wrote mip 0 and left coarser mips stale; the visual test was green because every mip was uniformly stale. |
| [gpu-scan-compaction.md](gpu-scan-compaction.md) | A compaction test that sorts both sides passes identically on `atomicAdd` and on the scan replacing it. |
| [variable-rate-compute-shading.md](variable-rate-compute-shading.md) | A classifier that coarsens nothing passes every "did coarsening damage the image" assertion. |
| [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md) | Every glyph invisible on AMD with 852 quads submitted; baking that would defend a blank UI forever. |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | A merged quad with U and V swapped still merges and draws, and five of six face directions look right. |
| [cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md) | Green on every CI run because a runner never has a warm cache, and the failing path needs a second load. |
| [observer-camera.md](observer-camera.md) | The frozen cut looks plausible when culling quietly follows the observer. |
| [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md) §14, §16, §18 | Every tenant and sweep was green while the first full virtual-geometry frame on Vulkan failed three ways; a barrier scope true about the draws was false about the queue; one field answered two questions that disagree on the frame an attachment is created. |
| [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md) | A missing VNDF weight renders believable, permanently too-bright specular, smallest where you would check; two channels seeded with the PRNG increment correlate at +0.55 while passing every per-channel metric; a resolve with crossed accumulators stays plausible. |
| [screen-space-denoiser-chain.md](screen-space-denoiser-chain.md) | A half-resolution filter guided by the full-resolution G-Buffer rejects every tap along every silhouette, and the noisy fringe it leaves looks like the trace being bad rather than the guide; a history sized at the wrong band smears uniformly instead of failing. |
| [pixel-error-mesh-lod.md](pixel-error-mesh-lod.md) | Projecting through the real view-projection instead of a facing plane passes every value test; only an invariance test under camera direction separates them. |
| [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md) | A vertical wall at the tile edge, and six flat-coloured islands, with every pipeline stage verified correct; "assert the weights vary" passes on the bug. |
| [water-shading-nyquist.md](water-shading-nyquist.md) | Normals derived without the amplitude the displacement carried, for months; a second FFT grid lost 38% of slope RMS while height RMS held. |
| [persistent-world-space-fields.md](persistent-world-space-fields.md) | A wake in an R8 texture renders and follows correctly and never fades, because the decay step rounds to zero. |
| [compute-in-place-vs-ping-pong.md](compute-in-place-vs-ping-pong.md) | Adding a neighbour read to an in-place compute pass races between work groups; it renders, in bands that follow the dispatch order, and looks like a bug in the maths you just wrote. |
| [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md) | Shader and CPU agreed on the function and disagreed on which space its argument was in; four of five evidence cameras pointed away from the boat. |

**The counter-move:** name the observation that would have failed. Usually it is a moving target
instead of a static one, an edge instead of a steady state, a second camera angle, or a physical
quantity in pinned units rather than a composited pixel.

## 2. One contract, several mirrors

The same fact written in more than one place, with nothing enforcing agreement.

| Doc | The mirrors |
|---|---|
| [distance-impostor-reflection-probes.md](distance-impostor-reflection-probes.md) | Header, bake and GLSL, pinned by a regex parity test. |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | Three light evaluators that can render the same scene. |
| [ddgi-probe-cascades-and-sparsity.md](ddgi-probe-cascades-and-sparsity.md) | The cascade-shift invalidation is derived independently on CPU and GPU, on purpose. |
| [reference-path-tracer.md](reference-path-tracer.md) | A C++ BRDF port against the GLSL it mirrors. |
| [baked-lightmap-pipeline.md](baked-lightmap-pipeline.md) | The GI stores' units ledger, and bake-time unwrap parameters mirrored by the runtime resolve. |
| [lightmap-receiver-identity.md](lightmap-receiver-identity.md) | Four walks that must gather the same lightmap receivers; a mismatch renders with no baked GI and no error. |
| [lightmap-receiver-identity.md §VirtualGeometry](lightmap-receiver-identity.md#virtualgeometry-the-uv2-rides-the-vertex-arena-because-no-binding-was-available) | A reserved vertex-pull binding resolves from the VAO, so publishing a buffer there is silently ignored. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | Five exclusion sets, each mirrored by a coverage-test roster. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | The build pipeline and the runtime must agree on an asset layout. |
| [audio-voice-budget.md](audio-voice-budget.md) | One config field costs four edits, one of them silent. |
| [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) | Two build trees writing the same generated files. |
| [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md) | A static destructor reading a `thread_local` that `__dyn_tls_dtor` already destroyed; 219 failures, one bug. |
| [concurrent-cmake-configure.md](concurrent-cmake-configure.md) | Two configures sharing one `CMakeFiles/`; the nested `try_compile` that loses reports the error. |
| [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) | Four subsystems hold world-space state outside the rebased set. |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | The packed-quad layout lives in `VoxelQuad.h` and `VoxelQuadUnpack.glsl`, and a mismatch compiles. |
| [destructible-debris.md](destructible-debris.md) | Two unrelated physics layer numberings; `SetCollisionLayer(Debris)` never reaches Jolt's `DEBRIS`. |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) | Four uint packings in C++ and four GLSL files; a wrong bit renders plausible wrong content. |
| [gpu-scene-record-contract.md](gpu-scene-record-contract.md) | Five records in `GPUSceneTypes.h` and `include/GPUScene.glsl`, pinned by `static_assert` and a SPIRV-Cross reflection test over member names, offsets and stride. |
| [gpu-scene-record-contract.md §7](gpu-scene-record-contract.md#7-the-raster-consumer-a-draw-link-resolved-once-per-frame) | Every raster path that has not migrated keeps its own previous-frame transform, and a second scene representation grows one convenient copy at a time. Pinned by `GPUSceneAntiDuplicationRatchetTest`, which requires every duplicating source file to be named in `GPUSceneLegacyAdapters.h` and every name there to still duplicate. |
| `ShaderBindingLayout.h` ↔ `include/BindlessHeap.glsl` | `HEAP_IMAGE_SLOT_BASE` is derived in C++ and a literal in GLSL, so adding any `TEX_*` slot is also a shader edit (#702). Pinned by `BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout`. |
| [ssbo-binding-cap-is-80-on-mesa.md](ssbo-binding-cap-is-80-on-mesa.md) | The SSBO namespace's ceiling lived in prose ("full at 84") copied from the UBO namespace; the driver's real number (80 on Mesa) was mirrored nowhere, so four bindings sat above it with every test green. Pinned by `SSBO_BINDING_LIMIT` + `static_assert` and `ShaderBindingLayout.SSBOSlotsFitTheMesaCeiling`. |

**The counter-move:** a parity test that reads both sides as text, or a generator that makes one side
derived. A "keep in sync" comment is not a mechanism.

## 3. It silently drops something

No crash, no error, no log line; work or data disappears and the system keeps running.

| Doc | What was dropped |
|---|---|
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | The neighbour rebuild after a carve, leaving a stale wall. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | A field, from every save. |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) §4 | An unmapping, once publishing became incremental. |
| [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) | A settings struct, on entering Play. |
| [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md) §7 | A virtual-shadow page request once the ring filled; the only evidence is slightly-too-soft shadows. |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | An authored light parameter, turned into a dead knob. |
| [reference-path-tracer.md](reference-path-tracer.md) §5 | DDGI's whole infinite-bounce term for any probe volume fitted to a room. |
| [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) | World position, gradually. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | Every script, in shipped games only. |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | A component's teardown, when `m_Registry.destroy()` skips `OnComponentRemoved`. |
| [vcpkg-dependency-management.md](vcpkg-dependency-management.md) | Three of five traps are silent, including a port option that never applied and one that switched on and killed TSan. |
| [asset-import-usd-alembic.md](asset-import-usd-alembic.md) | Winding, up-axis, unit scale, UV origin. |
| [asset-import-openvdb-volumetric.md](asset-import-openvdb-volumetric.md) | A fog volume rendered as a solid box through every non-VDB-aware evaluator. |
| [mcp-protocol-eras.md](mcp-protocol-eras.md) | Event pushes, when the notification carrier was swapped instead of run beside the old one. |
| [steamworks-platform-integration.md](steamworks-platform-integration.md) | The whole feature, when the SDK path is one level too high; the build succeeds with Steam off. |
| [configure-time-variable-visibility.md](configure-time-variable-visibility.md) | A DLL copy step for the test executable, on the first configure only. |
| [cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md) | A texture, from the second load onward. |
| [shared-atlas-allocator.md](shared-atlas-allocator.md) | A budget claim, when `vector::resize()` or a `= T{}` reset discards a non-RAII handle. |
| [technique-selection-seams.md](technique-selection-seams.md) | The REASON a light did not get the technique it asked for, when the choice is a shader branch: by the time the shader runs it is one uniform, and nothing on the CPU made the decision. |

**The counter-move:** ask what the absence would look like. If nothing would differ, you need a
coverage test, not a unit test.

## 4. Your instrument is lying to you

The check passes for a correct implementation and for a broken one.

| Doc | The instrument that failed |
|---|---|
| [live-verification-noise-floor.md](live-verification-noise-floor.md) | A crop check that a mirrored, wrong position scored better on; read tools that answer 200 with a stale frame from an iconified window. |
| [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md) | A GPU counter that stopped updating is byte-identical to one that is constant. |
| [incomplete-texture-samples-as-zero.md](incomplete-texture-samples-as-zero.md) | A sampled zero is a value, not an error: the frame is wrong exactly where the feature is active and right where it is not, on one vendor only. |
| [std-distributions-are-not-portable.md](std-distributions-are-not-portable.md) | Two platforms disagree about procedural content, or a test passes on one and fails on the other with no GPU difference behind it. |
| [procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md) | A red that recurs every run gets normalised and blinds the suite. |
| [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md) | A `glXxx(` grep is wrong three different ways. |
| [volumetric-cloud-debugging.md](volumetric-cloud-debugging.md) | Capture targets show the editor camera; include-only shader edits do not hot-reload; "darker" passes for every uniform veil. |
| [timed-wait-test-assertions.md](timed-wait-test-assertions.md) | `duration_cast<milliseconds>` truncates toward zero. |
| [shared-temp-dir-test-isolation.md](shared-temp-dir-test-isolation.md) | A CI comment asserted a safety property nobody had measured. |
| [cross-test-renderer-state.md](cross-test-renderer-state.md) | A visual test that passes 10/10 in isolation and draws nothing in the full run, with identical engine logs; two poisoners are each necessary and neither sufficient, and a reduced repro reproduces a different bug. |
| [reference-path-tracer.md](reference-path-tracer.md) | A golden answers "did it change", never "is it correct". |
| [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md) | A per-vendor baseline validates itself; a small cross-vendor RMSE measures portability, not correctness. |
| [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md) | Every obvious noise metric passes on white noise; only the error spectrum separates blue from white. |
| [gpu-debug-draws.md](gpu-debug-draws.md) | Read the two-counter overflow protocol before concluding "it drew nothing". |
| [static-archive-4gib-ceiling.md](static-archive-4gib-ceiling.md) | `LNK1248` reports the offset where the archive crossed 4 GiB, not its size. |
| [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) §5e, §6 | A CI job that said `--config Release` built no config; a cache hit restored objects without dependency files, so header edits rebuilt nothing. |
| [lazy-static-release-ownership.md](lazy-static-release-ownership.md) | GL has no allocator-teardown assertion, so a clean GL close proves nothing about teardown. |
| [incremental-build-odr-staleness.md](incremental-build-odr-staleness.md) | A correct fix that a live rebuild kept "disproving"; the binary was stale, not the source. |
| [pch-masked-missing-includes.md](pch-masked-missing-includes.md) | A green Windows build says nothing about a header's self-containment: the PCH is in scope everywhere, so 18 of 31 headers that failed a standalone compile built clean. |
| [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md) | A cache has no wrong-looking failure state: it fails by being slow. Every save in the repo had been refused for six days and every run stayed green. |
| [actions-cache-budget.md](actions-cache-budget.md) | The Windows sccache entry had not existed for weeks. Restore said "Cache not found", save said "your cache is now read only" and exited 0, and the only visible symptom was a build that took four hours. |
| [cache-entry-version-is-the-path-string.md](cache-entry-version-is-the-path-string.md) | The vcpkg key matched exactly and the entry was still invisible: restore and save spelled the same directory two ways, so every entry ever written had `last_accessed_at == created_at`. |
| [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md) | The build step takes compile time on a shader-only change while the cache directory is warm. A PCH-using compile is uncacheable without `CCACHE_SLOPPINESS`; a per-commit macro on every TU misses every run. |

**The counter-move:** measure the noise floor first, and construct a case the instrument must fail
on before trusting a case it passes.

## 5. Ordering and lifetime

The logic is right; when it runs, or how long it lives, is wrong.

| Doc | The ordering or lifetime rule |
|---|---|
| [script-structural-command-safe-point.md](script-structural-command-safe-point.md) | Never mutate the registry mid-iteration. |
| [visual-script-vm.md](visual-script-vm.md) | A loop node that forgets to charge its own iteration hangs the frame. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | Five ordering rules for a swap that destroys the thing being iterated. |
| [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) | Input before the physics kick, camera last. |
| [two-phase-occlusion-culling.md](two-phase-occlusion-culling.md) | Pass order decides who still sees previous-frame depth. |
| [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) | Clearing the LRU bit one step early evicts the whole cache every frame; a perspective face cannot be culled like an ortho level (§8). |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) | Touch a priority-ordered LRU in reverse (§5); coarse-to-fine fill is one dispatch per level with barriers (§3a). |
| [render-pass-published-state.md](render-pass-published-state.md) | Publish last, restore deliberately. |
| [registries-must-outlive-their-registrants.md](registries-must-outlive-their-registrants.md) | A lazily-created registry is destroyed BEFORE the namespace-scope statics whose destructors unregister from it. |
| [cluster-lod-simplification.md](cluster-lod-simplification.md) | A lock must outlive the level that created it. |
| [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md) | A read from a pooled resource whose lifetime already ended. |
| [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md) | TOCTOU between a decrement and a re-read. |
| [non-recursive-lock-self-locking-helper.md](non-recursive-lock-self-locking-helper.md) | A locked scope calling a sibling that locks the same non-recursive mutex; unlock the callee, don't move the caller. |
| [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md) | Promoting a per-tick local to persistent state. |
| [parallelizable-mover-systems.md](parallelizable-mover-systems.md) | Split a system at its write boundary. |
| [gl-clear-program-revalidation.md](gl-clear-program-revalidation.md) | What is bound when you clear. |
| [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) | A CPU write between two recorded draws is last-write-wins on Vulkan. |
| [vulkan-parallel-recording.md](vulkan-parallel-recording.md) | Two parallel items wrote one UBO object, or transitioned one subresource; the merge reports the second, the first renders the wrong cascade. |
| [vulkan-parallel-graph-recording.md](vulkan-parallel-graph-recording.md) | Prepared passes never form a production group, or disabled members make a valid group decline. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A per-instance BLAS loop builds one structure twice; a saturated GPU Scene generation makes a dead record look live. |
| [gpu-scan-compaction.md](gpu-scan-compaction.md) | A `barrier()` only some invocations reach. |
| [lazy-static-release-ownership.md](lazy-static-release-ownership.md) | A shared lazy static released from a conditional teardown. |
| [configure-time-variable-visibility.md](configure-time-variable-visibility.md) | A CMake variable read by a subdirectory processed before the line that sets it. |

## 6. It was never actually called

Code that exists, is tested, and has no production caller, or has more callers than the grep shows.

| Doc | The finding |
|---|---|
| [server-authoritative-networking-loop.md](server-authoritative-networking-loop.md) | Twenty tested networking classes produced nothing because the drive function had zero call sites. Grep for callers of the entry point, not for tests. |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | `AddAgent` / `SetAgentTarget` had zero production callers. |
| [audio-voice-budget.md](audio-voice-budget.md) | Sounds start from six call sites, not the two a grep suggests. |
| [substituted-seams-compound.md](substituted-seams-compound.md) | Both decal paths drew zero fragments for as long as they existed; a feature with no scene has no coverage. |
| [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md) §7 | The game scene never set `TessellationEnabled`, and no shipped scene ever set `UseImpostor`. A flag that appears only in its own test has no product coverage. |
| [render-pass-published-state.md](render-pass-published-state.md) | `MeshComponent { Primitive: 0 }` is `None`: an entity that never renders. |
| [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) §5 | A `Setup()` that early-returns while a feature is off freezes that decision into the frame-graph fingerprint. |

---

## Adding a doc here

The corpus grows about ten files a month, so style and size matter more than completeness.

1. **Write it when the bug is fixed.** The wrong hypotheses you held evaporate within a day.
2. **Title it as the rule.** "Drive a pinned vertex by velocity, not position", not "Cloth notes".
3. **Rule first, story second.** The first sentence says what to do; the failure follows.
   No metaphor where a literal phrase exists.
4. **Lead with what stayed green.** A failure a test caught usually needs no doc; these files exist
   for the failures tests could not see.
5. **Cite the issue number.**
6. **Add two links, both in this file:** one sentence in Part A under its subsystem, and one row in
   the Part B archetype it belongs to. Nothing goes into `CLAUDE.md`.
7. **Keep it under about 10 KB.** Past that it stops being read; split it or move history to an
   appendix.
