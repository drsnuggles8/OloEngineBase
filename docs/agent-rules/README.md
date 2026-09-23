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
- [glsl-shaders.md](glsl-shaders.md): the SPIR-V rules a shader must follow to compile: no bare uniforms, UBO bindings, MRT outputs, and never `.length()` on a storage buffer (§6b — Vulkan rejects it at pipeline creation, long after the SPIR-V validated). §1a: run `scripts/check_shader_extension_floor.py` after adding any `#extension` — the shader toolchain floor is Vulkan SDK 1.4.357.0 and Ubuntu's apt packages are a year short of it.
- [technique-selection-seams.md](technique-selection-seams.md): when a subsystem grows a second way of computing the same number, the choice is a value carrying the reason it is not what was asked for — not another shader `if`, and not a new row in a path enum.
- [engine-owned-containers.md](engine-owned-containers.md): new engine-owned sequence and string data is `TArray` / `FString`; the binding surface and every map keep `std::`. How to tell which side you are on, and why a relocation mistake is green on MSVC and red on libstdc++.
- [sonarqube-review-alignment.md](sonarqube-review-alignment.md): read before `/code-review` so local findings match the cloud profile.

## Testing and verification

- [testing-architecture.md](testing-architecture.md): which renderer layer or Functional axis a new test belongs to, the registration contract, and (§10) the pass-level Vulkan coverage every rendering feature owes plus the end-of-run banner that says whether a run exercised Vulkan at all.
- [../testing.md](../testing.md): why we test what we test; value heuristic, anti-patterns, retirement criteria.
- [vulkan-software-driver-ci.md](vulkan-software-driver-ci.md): check a software Vulkan driver against the ADR 0010 extension contract, not its API version, before assuming it can host the Vulkan suite — and pass `--olo-require-vulkan` in any job that runs it, or a skipped run reports green.
- [substituted-seams-compound.md](substituted-seams-compound.md): every substitution a test makes is a seam it stops testing, and they compound — including building the same object a different way.
- [no-silent-fallbacks.md](no-silent-fallbacks.md): a path that cannot do what it was asked says so loudly and countably; rank a fallback by whether the substituted value can be INDEXED, and lower every entry point a caller falls back to.
- [measurement-validity-and-sentinels.md](measurement-validity-and-sentinels.md): a measured number travels with a status saying whether it IS a measurement, because zero is a legal timing and a legal counter; classify in a pure function, make the backend report its refusals, and audit for fields that have a consumer and no producer.
- [reference-path-tracer.md](reference-path-tracer.md): the ground-truth oracle for "is it correct", where a golden can only say "did it change".
- [visual-quality-criteria.md](visual-quality-criteria.md): a criterion containing a judgement word — *convincing*, *natural*, *stable to the eye* — is settled by the frame, not by a proxy; enlarge before judging, measure the asset and not only the renderer, and require that a richness feature does not REDUCE measured fine detail.
- [resampled-estimator-measure-convention.md](resampled-estimator-measure-convention.md): write a resampled estimator's measure convention into the header that owns the sample, pin its Jacobian by an identity rather than an expected value, and add a negative control that fails if the term is removed.
- [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md): measure the noise floor and audit a recording before baking a per-vendor baseline.
- [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md): give a visual-test scene a ground plane, then look at the PNG.
- [live-verification-noise-floor.md](live-verification-noise-floor.md): measure frame-to-frame noise before attributing a pixel change, and confirm the editor is drawing at all.
- [procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md): a generator fix and its golden rebake ship in the same PR.
- [timed-wait-test-assertions.md](timed-wait-test-assertions.md): measure timed waits in microseconds and assert one-sided.
- [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md): keep the "is it still alive?" signal in a trivially destructible `thread_local`; a destroyed one is not readable.
- [shared-temp-dir-test-isolation.md](shared-temp-dir-test-isolation.md): use `TestTempDir.h`, never a fixed temp path; heavy cases run one per process and light suites one per process, concurrently.
- [cross-test-renderer-state.md](cross-test-renderer-state.md): never shut down a process-wide singleton you did not start, and leave the renderer configuration as you found it; plus the two traps that make single-process bisection lie.
- [world-anchored-renderer-state-in-tests.md](world-anchored-renderer-state-in-tests.md): a fixture that builds a `Scene` per test must also reset the process-static renderer state scene load resets.
- [ab-diff-peak-is-not-a-location.md](ab-diff-peak-is-not-a-location.md): place a measurement box by the highest-mean window of an A/B difference, never by its brightest pixel.

## Build and dependencies

- [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md): never build msvc and clangcl trees together; caches, link bounds, memory, the local ASan recipe.
- [build-memory-per-tu.md](build-memory-per-tu.md): set `--parallel` and every memory cap from the published per-TU peak-RSS ranking, not from a remembered number.
- [concurrent-cmake-configure.md](concurrent-cmake-configure.md): one configure at a time per build tree; the error blames your CMakeLists.txt and LTO instead.
- [static-archive-4gib-ceiling.md](static-archive-4gib-ceiling.md): a .lib cannot exceed 4 GiB, and `LNK1248` under-reports the overshoot.
- [vcpkg-dependency-management.md](vcpkg-dependency-management.md): read before adding, bumping or removing a dependency; the CRT triplet mismatch is heap corruption.
- [configure-time-variable-visibility.md](configure-time-variable-visibility.md): a CMake variable must be set before the `add_subdirectory()` that reads it.
- [asset-import-usd-alembic.md](asset-import-usd-alembic.md): the importer registry seam, and vendoring OpenUSD / Alembic / MaterialX statically.
- [asset-import-openvdb-volumetric.md](asset-import-openvdb-volumetric.md): keep OpenVDB editor-only; derive the grid transform without hand-transposing; extend every exhaustive `switch`.
- [groom-curve-import.md](groom-curve-import.md): polygon Alembic import is not curve support; reject malformed curve data instead of clamping it; the cook canonicalises and never resamples.
- [groom-strand-visibility.md](groom-strand-visibility.md): real hair is sub-pixel at every practical framing, so a strand is widened to one pixel and pays for it in alpha; model the square-ended quad the GPU draws, and refuse a stochastic mode that has no temporal resolve to converge it.
- [groom-scene-temporal-resolve.md](groom-scene-temporal-resolve.md): a scene holding a stochastic groom requests engine TAA every frame instead of storing it, and every pipeline site reads the one latched answer.
- [mesh-cache-must-return-the-surface-it-was-given.md](mesh-cache-must-return-the-surface-it-was-given.md): a cold import, a warm `.omesh` load and a pack load must give the same index buffer corner for corner, or every index-addressed consumer (a groom binding) breaks on whichever path loaded second.
- [groom-surface-binding.md](groom-surface-binding.md): a binding addresses triangles by index, so a topology mismatch refuses instead of rebinding; carry each strand rigidly from one frame construction; derive previous-frame positions from the previous pose rather than a carried-forward buffer.
- [groom-fibre-scattering.md](groom-fibre-scattering.md): a fibre is not a surface, so the per-light factor is the strand's projected width and never N dot L; averaging point samples across the fibre width reproduces the lobe as spikes, so widen each node to cover its own slice; energy conservation lives in the attenuations, not in the quadrature.
- [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md): the coat term is geometric and colourless, so the pigment is not applied twice; finer is WORSE for both representations, which alias below about two strand spacings; the selected volume is light-independent, so an animated light costs zero rebuilds.
- [groom-guide-simulation.md](groom-guide-simulation.md): enforce a strand's length by projection rather than by iteration, so the guarantee is a property of the algorithm and not of the tuning; integrate on a fixed step with a bounded, COUNTED catch-up, never a raw variable dt; a teleport re-seeds without also integrating, and a guide the strand budget did not select must still be added to the deformer or it is solved against the bind pose.
- [coat-authoring-and-per-role-budgets.md](coat-authoring-and-per-role-budgets.md): spend a groom's strand budget per coat role, weighted, or a budget cut takes the animal's outline first; key every coat decision on the root UV and the curve index so a regional map cannot slide under deformation; measure a multiplicative variation with a relative spread, not a variance.
- [groom-representation-lod.md](groom-representation-lod.md): compensate a thinned coat's width by 1/k and not 1/sqrt(k), because a strand is a band and not a sprite; move a budget in halvings so the cached geometry is not rebuilt every frame, and carry the continuity in the compensation instead; a cooked LOD level must map back to the base curve it stands in for, or the coat detaches from the body at exactly the distance nobody is watching.
- [groom-ray-traced-proxies.md](groom-ray-traced-proxies.md): a groom proxy occludes other receivers and never its own coat, because the coat's self-occlusion is already the density volume's; widen the proxy's radii by both the unit-scale lever and the thinning's 1/k, because the strand build applies neither; and reallocate rather than refill, because an undeformed coat is built once and compacted and a refill in place freezes its shadow for the session.
- [multi-animal-scheduling-budgets.md](multi-animal-scheduling-budgets.md): share one frame's work out among a POPULATION of animals, because every per-entity LOD ladder is right about its own entity and blind to the herd; spend the budget in calibrated units and never in a clock reading, or the same scene schedules differently on a busy machine and stops reproducing; bound a reduced animation tick rate in screen space rather than by distance, stagger the ticks by a per-entity phase or the mean improves while the frame-time tail does not, and report an unservable budget instead of absorbing it into the hero.
- [incremental-build-odr-staleness.md](incremental-build-odr-staleness.md): when a correct fix makes no sense live, suspect a stale incremental object before the code.
- [pch-masked-missing-includes.md](pch-masked-missing-includes.md): a header must include what it uses even when the build is green; the PCH hides the omission on Windows and only the Linux jobs name it.
- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md): a CI cache that restores is not
  one that works.
- [actions-cache-budget.md](actions-cache-budget.md): the Actions cache store is a fixed budget every key must fit in; past the cap it goes read-only and refuses every save in the repo, silently.
- [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md): set a compiler cache's size cap above the object set it has to hold, measured with `du`, then let the store's worst case overrule the number; and put whatever the key is hashed over into the key.
- [self-hosted-ccache-slot-multiplier.md](self-hosted-ccache-slot-multiplier.md): size a self-hosted compiler cache by configurations x the runner slots that can build them, make the slots share rather than paying for both, and let the filesystem overrule the number.
- [cache-entry-version-is-the-path-string.md](cache-entry-version-is-the-path-string.md): a cache's restore and its save must be handed the same path string, character for character; two spellings of one directory are two caches.
- [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md): print the cache statistics after every CI build and read the uncacheable line; a PCH without `CCACHE_SLOPPINESS` and a per-commit macro each made every engine object a miss.
- [shader-pack-bake.md](shader-pack-bake.md): the CI-baked `.osp` pack, its content-hash invalidation, and why a fresh worktree does not fetch it.
- [steamworks-platform-integration.md](steamworks-platform-integration.md): the SDK is developer-supplied, CI builds a stub, and exactly one TU may include a Valve header.

## Renderer

- [multi-mode-block-format-encoders.md](multi-mode-block-format-encoders.md): a block format with several block layouts is validated one mode at a time against the reference decoder's own read order; BC6H interpolates in half-float space, so a high-contrast block is not its hard case; a compute port keeps its integer half identical and returns bulk data in an `rgba32ui` image rather than spending a scarce buffer binding.
- [amd-mesa-shader-compile-blowup.md](amd-mesa-shader-compile-blowup.md): write a per-subset shader step as `subset 0; if (subsets > 1) subset 1` with constant array indices, never `for (s < subsets)` over indexed arrays; Mesa's AMD compiler took 14 GB and 200 s on the loop form while NVIDIA and llvmpipe took a second. Reproduce on the RADV null device; `ShaderCompileBudgetTest` and the per-process memory ceiling guard it.
- [incomplete-texture-samples-as-zero.md](incomplete-texture-samples-as-zero.md): set a texture's sampler state at creation; an incomplete texture reads zero on AMD and fine on NVIDIA.
- [std-distributions-are-not-portable.md](std-distributions-are-not-portable.md): seed procedural content with your own transform over mt19937; std:: distributions differ between standard libraries, so one seed is two different results.
- [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md): the OpenGL boundary leaks through the include graph, not a `glXxx(` grep; plus the Vulkan epic's lessons.
- [transparent-draw-order.md](transparent-draw-order.md): a conventional alpha-blended draw sorts depth-FIRST, which is a per-render-mode bit layout under the RenderMode bits and not a second comparator; auto-batching may only group blended draws that share a COMPLETE sort key; additive commutes and OIT is a different claim; and object-level sorting never resolves intersecting transparents.
- [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md): a CPU buffer write between two recorded draws is last-write-wins on Vulkan — and the per-draw snapshot that fixes it must cover the whole buffer or none of it, never a buffer the GPU produces, and every buffer TYPE with that shape (vertex streams were skipped for two years).
- [vulkan-large-vertex-stream-command-ordered.md](vulkan-large-vertex-stream-command-ordered.md): a vertex stream whose writes exceed half an arena slot is written by transfers recorded in command order instead of arena snapshots, which such a stream could never fit.
- [discard-is-not-a-bounds-check.md](discard-is-not-a-bounds-check.md): in a fragment shader, bound a GPU-driven index by clamping it — `discard` stops the fragment being written, not the dependent load being issued, and a root-data buffer read has no bounds.
- [vulkan-sampler-array-bindings.md](vulkan-sampler-array-bindings.md): map an array binding with the INDIRECT_INDEX_ARRAY source and write one heap index per element; the scalar source silently requires the array's textures to sit on consecutive heap slots, which nothing allocates.
- [vulkan-parallel-recording.md](vulkan-parallel-recording.md): a pass forks with `RenderCommand::RecordParallel` and gives every item its own resource objects; per-command-buffer state is per recording context.
- [command-packet-lifecycle.md](command-packet-lifecycle.md): write a command packet, bucket or `FrameDataBuffer` range only before its first replay; to vary a frozen packet, clone it and edit the clone.
- [vulkan-parallel-graph-recording.md](vulkan-parallel-graph-recording.md): schedule ready prepared passes before compiling resource lifetimes, and publish shared state only after joining.
- [vulkan-async-compute-queue.md](vulkan-async-compute-queue.md): a resource crossing between the graphics and async compute queues needs a matched release/acquire ownership pair, not just a semaphore; a compute-only queue rejects every graphics stage mask, and the no-async-queue degrade path is the one CI runs.
- [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md): a BLAS is per geometry and opacity is per instance; acceleration structures reach a shader as a device address, builds ride the frame command buffer, and compaction is a multi-frame handshake because idling is banned.
- [deformed-surfaces-in-acceleration-structures.md](deformed-surfaces-in-acceleration-structures.md): a ray tracer can only trace geometry that is in memory, and skinning happens in the vertex stage, so animated surfaces need a producer that writes the pose to a buffer before a BLAS can hold it.
- [vulkan-extension-adoption-claims.md](vulkan-extension-adoption-claims.md): check an extension's claims against the pinned SDK's `validusage.json` and `vk.xml` before building on them — an address-for-handle extension does not remove create-time usage bits, it can add one, and two extensions shipped for the same design are routinely independent in `vk.xml`.
- [vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md): a device-fault address means nothing alone — turn on device-fault, NV checkpoints, `OLO_VULKAN_ADDRESS_BINDING_REPORT=1` and `OLO_VULKAN_AFTERMATH=1` together so the report names the address, the pass, the owning object and — only Aftermath does this — whether that resource was already destroyed and which shader read it; never correlate those records with an engine-side log by handle, because the layer wraps non-dispatchable handles.
- [gpuav-rewrites-the-usage-it-judges-you-against.md](gpuav-rewrites-the-usage-it-judges-you-against.md): attribute a validation message to a layer configuration before changing engine code — GPU-AV's buffer validation adds `STORAGE_BUFFER_BIT` to application buffers and core validation then reports you for not declaring it.
- [vulkan-shader-heap-indexing.md](vulkan-shader-heap-indexing.md): a shader with no OpenGL twin may index the descriptor heap for any texture, and a shader that has one may index it for its five material-local maps on its Vulkan arm only; every other declaration keeps classic bindings on both backends.
- [gpu-path-tracer.md](gpu-path-tracer.md): the GPU reference path tracer mirrors the CPU one term by term, restarts its accumulation on any unjittered camera change or dirty GPU Scene range, and falls back structurally; a change to either tracer's transport is a change to both.
- [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md): `glColorMask` and `glEnable(GL_BLEND)` are indexed calls for every draw buffer; never port one as a fallback.
- [lazy-static-release-ownership.md](lazy-static-release-ownership.md): release a shared lazy static from an unconditional teardown, not from `Renderer3D::Shutdown`.
- [registries-must-outlive-their-registrants.md](registries-must-outlive-their-registrants.md): a process-wide registry a destructor unregisters from must be a deliberately leaked singleton, never a plain static.
- [gpu-debug-draws.md](gpu-debug-draws.md): any shader can draw a primitive into the viewport; read the overflow protocol before concluding "it drew nothing".
- [shader-tuning-constants-need-a-unit.md](shader-tuning-constants-need-a-unit.md): give every authored render constant a unit that does not change with camera range, cascade or resolution, and rename the field when its unit changes; the CSM depth bias was normalized cascade depth, so the engine default was 2-13 metres of world offset.
- [observer-camera.md](observer-camera.md): the frozen culling camera decides what is drawn, never how it looks.
- [gpu-scan-compaction.md](gpu-scan-compaction.md): no early return in front of a work-group scan; test compaction order, not sets.
- [variable-rate-compute-shading.md](variable-rate-compute-shading.md): measure departure from a plane, not depth range, and read the heatmap first.
- [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md): publish GPU counters by name without stalling; the buffer-binding namespace is full.
- [ssbo-binding-cap-is-80-on-mesa.md](ssbo-binding-cap-is-80-on-mesa.md): every `SSBO_*` binding stays below 80, because Mesa exposes 80 storage-buffer binding points and the UBO namespace's 84 does not bound them.
- [gpu-scene-record-contract.md](gpu-scene-record-contract.md): a GPU-scene record changes in C++ and GLSL in one commit, and only an incompatible edit or a removal advances its generation.
- [gpu-scene-record-contract.md §7](gpu-scene-record-contract.md#7-the-raster-consumer-a-draw-link-resolved-once-per-frame): a migrated draw carries a link to its record, resolved once after the commit, and every path that still duplicates transform truth is named in `GPUSceneLegacyAdapters.h`.
- [animated-surface-records.md](animated-surface-records.md): a record's deformation history is copied verbatim from the producer that owns the palettes, never derived from the slot the way its transform history is; identity is per submesh and the revision is per entity.
- [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md): blue noise is a claim about the error spectrum; the VNDF weight fails silently; a resolve clips, not clamps.
- [temporal-reactivity-separation.md](temporal-reactivity-separation.md): carry history rejection as three separated causes rather than one confidence, give the coverage term a dead band sized to the estimator's own noise, and measure ghosting, shimmer and detail loss across a sequence because none of them is visible in a settled frame.
- [temporal-measurement-control-arms.md](temporal-measurement-control-arms.md): a control arm that reads exactly zero is broken rather than clean, "feature off" is not a control when the engine reconfigures around the feature, and a subject-presence guard has to measure the subject against a capture without it.
- [screen-space-denoiser-chain.md](screen-space-denoiser-chain.md): a filter that runs at half resolution is guided by a guide buffer at its own resolution, its geometry test is a plane distance, and only a stage's SIZE is graph topology.
- [gl-clear-program-revalidation.md](gl-clear-program-revalidation.md): wrap every new clear site in `GLClearProgramGuard`, unbind and restore.
- [render-pass-published-state.md](render-pass-published-state.md): a pass that publishes engine-global bindings runs last and is not wrapped in `GLStateGuard(Restore)`.
- [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md): `WriteNewVersion` renames a physical resource; use the poison and disable levers to find stale reads.
- [render-pipeline-caches.md](render-pipeline-caches.md): process-wide render caches invalidate on every topology reset, not only on a fingerprint change.
- [render-graph-setup-declaration-gates.md](render-graph-setup-declaration-gates.md): a pass whose `Setup()` declares nothing when it has no work must report that gate in the declaration key, or it stays culled while its bucket fills.
- [render-graph-declaration-config.md](render-graph-declaration-config.md): anything `Setup()` or `PopulateBlackboard` branches on is a `FrameGraphDeclarationConfig` field or a pass's `AppendDeclarationInputs`, read from where the key reads it; execution-only data, history generations included, stays out.
- [ddgi-probe-cascades-and-sparsity.md](ddgi-probe-cascades-and-sparsity.md): the DDGI clipmap is toroidal, `%` truncates toward zero, and sparsity fails as "no GI, no error".
- [baked-lightmap-pipeline.md](baked-lightmap-pipeline.md): the GI units ledger, UV2 as a parallel stream, and keying the seam split after the unwrap.
- [lightmap-receiver-identity.md](lightmap-receiver-identity.md): a baked lightmap region is addressed by (entity UUID, sub-key), never by the entity alone.
- [two-phase-occlusion-culling.md](two-phase-occlusion-culling.md): phase 1 tests the previous frame's final pyramid; pass order decides who sees old depth.
- [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md): four page-cache invariants; a `Setup()` that branches on a runtime toggle is frozen by the fingerprint cache.
- [virtual-geometry-into-a-second-shadow-technique.md](virtual-geometry-into-a-second-shadow-technique.md): a caster family reaches a shadow technique only if somebody wired it there, and the gap is invisible.
- [cluster-lod-simplification.md](cluster-lod-simplification.md): a terminal group's boundary lock outlives the level that created it, and a terminal group is marked `FLT_MAX`, which is finite.
- [deforming-geometry-conservative-bounds.md](deforming-geometry-conservative-bounds.md): a bound that only rejects may be tightened per cluster, but a bound a hierarchy's nesting or monotonicity invariants rest on grows by one uniform scalar, derived rather than estimated.
- [pixel-error-mesh-lod.md](pixel-error-mesh-lod.md): the LOD plane faces the mesh, not the camera, and the error metric must be a ratio.
- [compute-written-texture-mip-chain.md](compute-written-texture-mip-chain.md): every writer of mip 0 owes the rest of the chain.
- [terrain-gpu-lod-quadtree.md](terrain-gpu-lod-quadtree.md): crack-freedom is a vertex-set property; picking must not inherit the tessellation gate.
- [terrain-virtual-texturing.md](terrain-virtual-texturing.md): every VT defect is a wrong address; touch LRU in reverse priority; an eviction is an entry, not an absence.
- [async-fault-in-under-a-residency-cache.md](async-fault-in-under-a-residency-cache.md): obtain the payload before you claim the slot, or the cache evicts a live entry for bytes that have not arrived.
- [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md): measure a tile's outermost ring, and never key an auto-material rock rule below the shoreline mask's slope.
- [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md): the packed-quad encoding is mirrored in GLSL, and a merged quad can render plausibly and wrong six ways.
- [camera-relative-rendering.md](camera-relative-rendering.md): every world-space GPU upload is a site; f32 cancellation shows as jitter and shadow swim.
- [distance-impostor-reflection-probes.md](distance-impostor-reflection-probes.md): one encoding contract mirrored in three places, and a miss sentinel that shades from stale sky.
- [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md): three ways impostor cards go missing, separable only by reading PNGs from several azimuths; and a layer draws two shapes, so read `EnumerateLayerDraws` rather than any one pass's shader.
- [vegetation-asset-import.md](vegetation-asset-import.md): a texture resolution tier does not shrink the mesh, modelled foliage cannot be decimated so bake its cards out of the scan, and measure alpha coverage over the drawn surface rather than over the texture sheet.
- [foliage-lod-transition-coverage.md](foliage-lod-transition-coverage.md): a popping LOD ring is a dispersion of the crossing instances, not a count of them; put a thinning fade band above the keep threshold and compensate the effective keep fraction, not the raw one.
- [foliage-hierarchical-wind.md](../guides/foliage-hierarchical-wind.md): share deformation across raster passes, bound square impostor corners, and evaluate each motion-history facing basis independently.
- [foliage-interaction-bending.md](../guides/foliage-interaction-bending.md): express a recovery in seconds and integrate the spring analytically, not as a per-frame lerp; the influence set is the switch, so an empty one must contribute exactly zero.
- [procedural-scatter-hash-and-habitat-rules.md](procedural-scatter-hash-and-habitat-rules.md): judge a placement hash by its distinct-offset count, keep gating rules out of the placement signature and moving ones in, and default every new scatter rule to the multiplicative identity.
- [capture-paths-take-the-projection-seam.md](capture-paths-take-the-projection-seam.md): every matrix a vertex stage feeds to `gl_Position` goes through `RHIProjectionSeam`, a bake's private ortho included, and a capture that skips the y flip draws with culling off.
- [light-path-photometric-parity.md](light-path-photometric-parity.md): the three light evaluators must agree; a dropped GPU struct field is a dead knob.
- [volumetric-cloud-debugging.md](volumetric-cloud-debugging.md): eight causes of a uniform veil, and how to tell "darker" from "directionally darker".
- [water-shading-nyquist.md](water-shading-nyquist.md): a derived normal carries every factor its displacement carries; drop sub-pixel detail rather than filter it.
- [geometry-lod-measure-the-unreachable-cost.md](geometry-lod-measure-the-unreachable-cost.md): count the invocations paid upstream of a shader-stage cull, and measure pixel area rather than distance, before redistributing geometry.
- [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md): the shared thing between a shader and gameplay sampling must be an analytic record, in one agreed space.
- [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md): deform a skinned vertex in one shared include that every pass calls, and advance its previous-pose history once per frame for every skinned entity — inside the gameplay tick it stops while paused, and a paused character then smears for as long as the pause lasts.
- [morph-and-lod-in-the-animated-surface.md](morph-and-lod-in-the-animated-surface.md): apply morph deltas to the rest surface before skinning, reject the deformation history of a surface that moved instead of emitting a velocity for it, and resolve an animated entity's conventional LOD level once per frame so the pass that deforms and the pass that draws mean the same mesh.
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
- [derived-graph-must-outlive-its-source.md](derived-graph-must-outlive-its-source.md): before answering "what points at X" from an existing graph, enumerate its writers and check the direction of every edge; and a direction bug can be invisible because its reader is wrong the same way, so change both halves together.

## Gameplay, physics and simulation

- [force-model-vehicles.md](force-model-vehicles.md): boats and aircraft driven by `AddForce`; every bug here leaves the suite green.
- [jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md): drive a pinned cloth vertex by velocity, never by position.
- [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md): a `CharacterVirtual` is invisible to UUID-keyed body filters, and a follow camera runs last.
- [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md): a valid navmesh silently switches every `NavAgentComponent` onto the crowd follower.
- [parallelizable-mover-systems.md](parallelizable-mover-systems.md): how to move an entity-moving system onto a worker thread, and the two traps in its determinism test.
- [terrain-collision-streaming-sculpt.md](terrain-collision-streaming-sculpt.md): streamed tile bodies live in a second `JoltScene` map, and tile pose must match the draw transform exactly.
- [procedural-skinned-mesh-primitives.md](procedural-skinned-mesh-primitives.md): code-built test meshes need a live GL context and a shared bind-pose origin.
- [destructible-debris.md](destructible-debris.md): pre-authored debris chunks, and two unrelated physics layer numberings.
- [structural-collapse.md](structural-collapse.md): anchors are explicit, sideways load transfer is charged for so a wall can partially collapse, and a static piece can only be made dynamic if its body was built expecting it.
- [audio-voice-budget.md](audio-voice-budget.md): admit the voice cap inside `Play()`, because sounds start from six call sites.

## Scripting, networking and tooling

- [script-structural-command-safe-point.md](script-structural-command-safe-point.md): a script binding that changes the registry structurally queues a command, never acts inline.
- [visual-script-vm.md](visual-script-vm.md): `Trigger` queues a branch, it does not run one (§1), so a loop node charges its own iteration and keeps it in `NodeState`; memoization is per exec step, and `PinType` numbering is on disk.
- [runtime-scene-switching.md](runtime-scene-switching.md): the host applies a scene swap after the tick; five ordering rules and the `Project` mount.
- [server-authoritative-networking-loop.md](server-authoritative-networking-loop.md): grep for callers of the entry point, not for tests.
- [mcp-setter-based-field-registry.md](mcp-setter-based-field-registry.md): copy-then-swap MCP writes are unsound when `operator=` cannot reproduce a setter's side effects.
- [mcp-protocol-eras.md](mcp-protocol-eras.md): the stateless core is a second transport; adding `server/discover` alone breaks working clients.
- [automation-build-invocation.md](automation-build-invocation.md): a build started from inside the editor goes through `build-lock.ps1` or it does not happen, the editor process is the lock's identity, cancellation kills the job object rather than the shim, and `OloEditor` is refused by allow-list.
- [automation-event-bus.md](automation-event-bus.md): an event carries identities, never a read's content; a subscriber holds a cursor into the one 512-record ring and is told the count it lost; only a mutating command publishes its completion.

## Concurrency and memory

- [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md): a decrement-then-reread of a refcount is a double-free even with atomics.
- [non-recursive-lock-self-locking-helper.md](non-recursive-lock-self-locking-helper.md): fix the callee that locks internally; don't wrap a self-synchronised member in an outer lock.
- [spinlock-payload-cache-line-separation.md](spinlock-payload-cache-line-separation.md): keep a lock off its payload's cache line.
- [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md): three checks before promoting a per-tick scratch vector to persistent state.
- [bump-allocator-rollover-padding.md](bump-allocator-rollover-padding.md): a rollover must not reserve padding computed for the block it is leaving; over-align the blocks instead.

## Subsystem notes (`notes-*.md`)

Accumulated per-subsystem gotchas. Skim the relevant one before working in that area.

- [notes-renderer.md](notes-renderer.md): offline capture, GL wrappers, shader bindings, SSAO/SSR/FSR, IBL bakes, GPU timers, which per-instance bounds list survives frustum culling, why two uniform buffers on one binding point is last-created-wins, why tightening a loop bound moves every guard that was calibrated against it, the four separate Scene draw sites a world-space 2D component must be wired into, and why a texture built in memory is vertically mirrored relative to one loaded from a file, why a regenerate-wholesale system keys identity on the generator's input slot rather than its output buffer row, and why every `TerrainData` height query syncs from the GPU, why a material field added to `PBRMaterialUBO` has to be declared in all eight GLSL blocks that mirror it, and what a new field costs in the G-Buffer flags lane, why a two-sided surface's shadow lookup has to be biased along the LIT-side normal, and why G-Buffer RT5 is a free per-pixel lane for a kind-gated scalar — but only unconditionally for the FIRST tenant. Also: why an approximation's error terms have to be measured together rather than one at a time, why a tripwire naming a fixed enum version cannot notice a new one, and why a cumulative version list belongs in one named predicate rather than nine copies.
- [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md): the three-part tool split, schemas, consent and undo, declaring `AutomationUndo` so the command can be batched at all, frame capture, why a newly registered tool is not in the default `tools/list`, and how its name and toolset decide its `oloctl` spelling.
- [notes-core-and-threading.md](notes-core-and-threading.md): yaml-cpp decode, `Ref<T>` constness, the task system, EnTT first-touch, C++ traps, and why `constinit` on a `std::vector` cannot compile in a Debug build.
- [notes-gameplay-physics-nav.md](notes-gameplay-physics-nav.md): the two Jolt systems, joints, Detour, dialogue guards, the gameplay scheduler.
- [notes-audio-animation-sim.md](notes-audio-animation-sim.md): pose sampling, retargeting, morph targets, the fixed-timestep split, SoundGraph.
- [notes-editor-and-assets.md](notes-editor-and-assets.md): Content Browser, filewatch import, placeholders, texture cook, the ScriptCore build edge, why a second meaning for an already-registered extension loses silently, and why an asset serializer that resolves a second asset used to deadlock the whole editor with no diagnostic at all. Also: which editor "graph panels" actually have a canvas, because three of the six named in the GraphCanvas migration never did, and what the three real migrations cost and fixed. Also: why a field only round-trips if you tested the serializer you did not write.

---

# Part B: by failure mode

## 1. The suite is green and the feature is broken

The dominant archetype here. If your change is in one of these areas, a passing run is not evidence.

| Doc | What stayed green |
|---|---|
| [force-model-vehicles.md](force-model-vehicles.md) | A boat with no thrust still floats and an oscillating aircraft still has finite positions. |
| [vulkan-software-driver-ci.md](vulkan-software-driver-ci.md) | A CI job that runs the Vulkan suite on a driver below the ADR 0010 contract: every device-gated test skips, gtest prints `[  PASSED  ]`, the job is green and nothing about Vulkan was verified. Mesa 26.1.8's lavapipe is the trap's live example — it clears the Vulkan 1.4 bar everyone checks and exposes none of the three extensions that actually decide it. |
| [jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md) | Unit tests pass whether the cape detaches, jitters or freezes rigid. |
| [foliage-hierarchical-wind.md](../guides/foliage-hierarchical-wind.md) | Reusing the current camera-facing card basis for previous positions loses wind-induced orientation motion even with a stationary camera. |
| [foliage-interaction-bending.md](../guides/foliage-interaction-bending.md) | A per-frame lerp recovery converges at a different speed on every machine and rings across a frame spike, and neither shows up in a single-frame capture. |
| [foliage-interaction-bending.md](../guides/foliage-interaction-bending.md) | An influence that follows an actor instead of being planted makes the grass snap upright the instant it passes, which reads as the recovery being broken rather than absent. |
| [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md) | Three separate bugs each rendered a plausible frame that read as "impostors missing". |
| [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md) | A foliage layer draws its authored mesh near and its card far; a pass that decides that for itself renders a plant whose shadow is a different shape. |
| [vegetation-asset-import.md](vegetation-asset-import.md) | A foliage layer whose albedo is a picture of a different species reads as four separate renderer bugs — see-through trees, needles that are streaks, a stippled far field, useless impostors — and is none of them. |
| [vegetation-asset-import.md](vegetation-asset-import.md) | Alpha coverage averaged over a texture SHEET reported 69.5% where the canopy sampling it reads 54.4%; a UV atlas's unused space is opaque, so the flat figure describes a canopy that does not exist. |
| [vegetation-asset-import.md](vegetation-asset-import.md) | glTF and OBJ disagree about which way v runs, and a flipped v baked brown pine cones onto every foliage card — a result that looks like a plausible dead tree and passes every geometric check. |
| [foliage-lod-transition-coverage.md](foliage-lod-transition-coverage.md) | Decorrelating a LOD threshold cannot reduce how many instances flip per frame — crossings per metre are conserved — so a test that asserts a smaller count fails while the feature works, and a pixel RMSE over a walking camera measures parallax instead. |
| [foliage-lod-transition-coverage.md](foliage-lod-transition-coverage.md) | A thinning fade band placed below the keep threshold thins the layer before its density band begins; the error is 0.04% at full density and invisible in every screenshot. |
| [procedural-scatter-hash-and-habitat-rules.md](procedural-scatter-hash-and-habitat-rules.md) | A scatter hash whose two jitter draws differ by a constant: 32 distinct offsets over 6400 cells, Pearson correlation 0.02, and a screenshot that looks random. |
| [capture-paths-take-the-projection-seam.md](capture-paths-take-the-projection-seam.md) | An off-screen bake uploaded a raw GL-convention ortho; on Vulkan every triangle sat at negative clip z and was clipped, so the atlas baked "successfully" as its clear colour and the whole impostor canopy rendered nothing — with no counter or validation error to show for it. |
| [mesh-cache-must-return-the-surface-it-was-given.md](mesh-cache-must-return-the-surface-it-was-given.md) | A horse's coat stood at its bind pose while the horse walked, but only in the editor. Every headless case imported the body fresh, and the warm-cache test compared counts, which survive a corner rotation. |
| [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md) | A bright material rendered near-black and the test asserted nothing about it. |
| [visual-quality-criteria.md](visual-quality-criteria.md) | Three correct measurements — coverage 39.3% vs 27.2%, luma 54.68 to 80.99, motion 11.61% vs 1.39% — marked "convincing geometry" met on a frame a human called 1998. Every one was a PRESENCE test, and enabling the feature that adds geometric richness had in fact REDUCED fine detail to 0.31x. |
| [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) | Settings reset on Play, and headless tests never call `Scene::Copy()`. |
| [world-anchored-renderer-state-in-tests.md](world-anchored-renderer-state-in-tests.md) | A visual golden passes in file order and fails in a shard, because it encoded the previous test's residue. |
| [ab-diff-peak-is-not-a-location.md](ab-diff-peak-is-not-a-location.md) | An A/B visual assertion reads BACKWARDS in one test ordering, from frames that are identical to the byte. |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | Two lighting bugs survived 4300 green tests. |
| [measurement-validity-and-sentinels.md](measurement-validity-and-sentinels.md) | Four unrelated failures — a dropped query ring, a timestamp the backend refused on a recording worker, a backwards cross-queue pair, and a device with no timestamp queries at all — all published `0.0 ms`, which every panel, MCP tool and persisted benchmark export reported as a pass that cost nothing. A checked-in study measured a 26% recording-time win and could claim no GPU number to go with it. |
| [engine-owned-containers.md](engine-owned-containers.md) | A `TArray` holding a type that cannot survive a bitwise relocation. libstdc++'s `std::string` keeps an SSO self-pointer and MSVC's does not, so the heap corruption is invisible on this box and aborts only on Linux CI. |
| [shader-tuning-constants-need-a-unit.md](shader-tuning-constants-need-a-unit.md) | Every directional shadow in the project sat metres away from its caster, and the tests pinned the constant's VALUE, which had not changed. |
| [glsl-shaders.md](glsl-shaders.md) §1a | A new `#extension` is green on every PR check and red on the next nightly: same-repo PRs used to route every Linux sanitizer job to the box that has the current SDK, so the arm with the older toolchain was only ever reached by the nightly — which then fails ~4,000 tests in, as `'descriptor_heap' : unrecognized layout identifier` against a line in an include file. |
| [glsl-shaders.md](glsl-shaders.md) §1 | Adding a padding lane called `_padding0` to a shared UBO block fails SPIR-V with `nameless block contains a member that already has a name at global scope` — a hard Vulkan-only error that every OpenGL test is blind to, in three shaders at once. |
| [amd-mesa-shader-compile-blowup.md](amd-mesa-shader-compile-blowup.md) | The shader compiled in a second on NVIDIA and on llvmpipe and every PR check was green; only the AMD nightly, the one run that hands a shader to Mesa's AMD compiler, saw 14 GB per test process, and it reported that as six `Subprocess killed` cases and a dead runner, not as a shader problem. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | A corrupt drive mode clamped to a different valid mode, and the car still drove. |
| [asset-degradation-and-constructor-preconditions.md](asset-degradation-and-constructor-preconditions.md) | "Load the scene, does it crash?" passes because the trigger is resolution, not loading. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | An extension already in `s_ExtensionMap` routes the file to the OLD importer, so a new format fails as a corrupt version of the old one rather than as an unsupported format. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | A serializer that calls `AssetManager::GetAsset` from inside its own `TryLoadData` re-enters the importer's non-recursive registry mutex: the editor freezes mid-frame with no crash, no assert and no log line, looking exactly like a slow shader warmup. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | The MCP viewport override is in pixels but lands in the field the DPI scale multiplies, so on a scaled display it renders a scene band larger than the window and the render graph stops resolving scene colour/depth — permanently, not just for that frame. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | A panel ranked into the `GraphCanvas` migration by `.cpp` line count turns out to have no pan, zoom, grid or wires at all, so the "migration" would mean adding a canvas that never existed; `AnimationGraphEditorPanel`, `FSMEditorPanel` and `BehaviorTreeEditorPanel` are forms and inspectors, not canvases. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | A node-editor pin hit radius that scales with zoom becomes a sub-2px target at minimum zoom, and a context-menu position captured in screen space drops the new node somewhere else if the view moves before the popup draws; both survived in all three hand-rolled canvases because nothing tests viewport maths. |
| [notes-core-and-threading.md](notes-core-and-threading.md) | `constinit` on a `std::vector` is a hard compile error in any Debug build here — MSVC's debug STL allocates an iterator-debug proxy in the default constructor — so a static-init audit that marks one breaks the whole tree at the library, nowhere near the code that reads it. |
| [notes-editor-and-assets.md](notes-editor-and-assets.md) | When two serializers cover one type, a field only round-trips if you tested the one you did NOT write: `MaterialAsset`'s YAML saved the typed PBR maps and loaded them into the generic uniform map, so a material asset came back with every map null while the scene path — which uses the typed setters — was fine. |
| [notes-renderer.md](notes-renderer.md) | `Texture2D::Create(path, …)` NEVER returns null — a file that will not open still yields a Ref, and `IsLoaded()` is the only thing that says so; a `if (!texture)` load check can never fire and the shader samples a broken texture. |
| [notes-renderer.md](notes-renderer.md) | A two-sided surface lit from BEHIND self-shadows to black: the cascade's receiver normal-offset walks the sample behind the surface's own depth, and the term the shadow was gating (transmission, translucency) disappears — reading as a broken lobe when it is a sign error. |
| [notes-renderer.md](notes-renderer.md) | A pass that needs one more per-pixel scalar can use G-Buffer RT5's rgb with coverage 0 and change no existing reader — but gate the READ on something only your writer sets, because a material KIND is authorable on ordinary meshes that write real irradiance there; and a SECOND tenant whose own writer can be lightmapped must let the irradiance win, or it trades a visible lighting regression for a subtle gain. |
| [notes-renderer.md](notes-renderer.md) | Two uniform buffers on one binding point: the shader reads the one constructed LAST, so the other's SetData lands where nothing reads and the frame renders with a zeroed camera. |
| [notes-renderer.md](notes-renderer.md) | A new world-space 2D component wired into only some of Scene's four `Renderer2D::BeginScene` sites draws in one viewport mode and silently vanishes in another; the editor's 3D-mode site is the one usually missed, and it is the path headless visual-evidence tests exercise. |
| [notes-renderer.md](notes-renderer.md) | The image loader flips rows on load but `Texture2D::SetData` does not, so an atlas built in a test is vertically mirrored against the same image on disk and every sub-rect samples its neighbour; a "did anything draw" assertion cannot see it. |
| [notes-renderer.md](notes-renderer.md) | Several passes estimating ONE quantity (reflections, AO, indirect diffuse) must composite bottom-up by confidence with the bottom tier pinned at 1, or every tier needs the confidence of the tiers above it; and a tier whose enable flag is missing from the graph fingerprint stays culled with every counter reading a truthful zero. |
| [notes-renderer.md](notes-renderer.md) | An approximation's two *named* error terms were both real, both measured, and neither was the dominant one: repairing them perfectly and for free moves the image FURTHER from ground truth, because the unnamed third term (separability) is larger than both and has the opposite sign. |
| [notes-renderer.md](notes-renderer.md) | …and the sequel: the three terms were COUPLED, so repairing either side alone was a regression and repairing both was a 3x win. A control that removes one term at a time can report that none of them matter. |
| [notes-renderer.md](notes-renderer.md) | A tripwire test written to fail when a decision is superseded stayed green through the change that superseded it: it named a fixed enum version, so it went on measuring the old one after the new one shipped. |
| [notes-renderer.md](notes-renderer.md) | A cumulative version list copied to nine sites went stale three times, always at the editor-inspector copy — the one `OloEditor` not being in the test build makes unreachable by any evidence test. |
| [notes-renderer.md](notes-renderer.md) | A system that regenerates its instances wholesale has no identity to hand downstream: the plant IS its instance-buffer row, so every regeneration renumbers every plant and nothing notices, because a renumbered plant renders identically. |
| [notes-renderer.md](notes-renderer.md) | `TerrainData::GetHeightAt` / `GetNormalAt` each call `SyncFromGPU()`, so a per-cell placement loop pays two staleness checks per cell and cannot run headless at all. |
| [notes-renderer.md](notes-renderer.md) | A terrain rebuild marked the splatmap and collision dirty but not the foliage on the same entity, leaving every plant at its old height on slopes the new terrain no longer has. |
| [notes-renderer.md](notes-renderer.md) | `PBRMaterialProperties` is declared in eight shaders with no shared include; a field added to seven of them relayouts the eighth's trailing heap offsets, and that pass samples a neighbouring descriptor for every material texture while compiling, linking and drawing normally. |
| [notes-renderer.md](notes-renderer.md) | The G-Buffer flags lane's closure-model field is open-ended on purpose, so a new field can only go below it — which halves the model's exact-representation ceiling per bit, and needs clamping on encode or an overflowing slot changes the closure model instead. |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | A test believed it exercised the manual path while a valid navmesh had switched it to the crowd follower. |
| [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) | A steady-state offset check passes with a full one-tick lag present. |
| [parallelizable-mover-systems.md](parallelizable-mover-systems.md) | A position check passes on the scheduler tie-break alone, with the dependency edge missing. |
| [multi-animal-scheduling-budgets.md](multi-animal-scheduling-budgets.md) | Forty animals each independently concluded they deserved full rate, because nothing in the engine arbitrated between them — and a budget that then coarsened them all onto the same tick frame lowered every mean while leaving the stutter exactly where it was. |
| [mcp-protocol-eras.md](mcp-protocol-eras.md) | Adding `server/discover` alone keeps tests green and breaks the legacy fallback for real clients. |
| [automation-event-bus.md](automation-event-bus.md) | The push stream served every event record unredacted while `olo_events_tail` served the same record scrubbed, and a cursor that fell behind the ring received a shorter history with no sign anything was missing. |
| [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md) | `tools/list` serves an exposure profile, not the registry, so a tool you just registered is callable but invisible — including to the agent you attach to check your work — and a test that registers a fake tool and lists it gets an empty array. |
| [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md) | An automation command that never declares `AutomationUndo` is silently refused from every transaction: the build is green, the tool works on its own, and the only symptom is a batch refusal read months later. |
| [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) | Two scenes rendered skybox-only with zero errors because no test interleaved two SSBO uploads with draws. Later, the same snapshot applied to a GPU-output buffer made the mesh-shader raster arm launch `EmitMeshTasksEXT(0)` — a legal call, so nothing warned — while masking an out-of-bounds read that device-faults once you remove it. Later still, vertex streams turned out never to have been given the snapshot at all, under a comment asserting nothing streamed them. |
| [discard-is-not-a-bounds-check.md](discard-is-not-a-bounds-check.md) | Every index in the software-raster resolve was guarded, every guard was a `discard`, and every CPU-side value read back valid — while the shader device-faulted on the cleared-pixel sentinel that most of the screen carries. The pass had never executed on Vulkan at all, so nothing had ever been green *or* red about it. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A new `RHI::Access` write member is classified as a read by the one switch with a `default:`, so no write-after-write barrier is emitted and nothing warns. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A BLAS build handed `firstVertex = BaseVertex` over already-global indices applies the offset twice: every non-first submesh traces triangles that are not there, and only the submesh that runs past the buffer turns it into a device loss. Legal API usage, so no validation layer sees it. |
| [vulkan-async-compute-queue.md](vulkan-async-compute-queue.md) | A queue-family ownership transfer that names its two families on only one half leaves the resource's contents undefined — on AMD. NVIDIA renders the correct frame, the validation layers say nothing, and there is no pixel to look at. |
| [vulkan-extension-adoption-claims.md](vulkan-extension-adoption-claims.md) | An issue's claim that a new extension removes a create-time constraint is trusted and built on; the VUIDs still require every usage bit and the conversion adds one. A "this does not narrow the hardware floor" claim derived from one device reaches an ADR before anyone reads the two `depends` strings that disprove it. |
| [vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md) | A device fault reports a bare address, so the object it belonged to is guessed at for hours; correlating the owner report with an engine-side destroy log by handle finds nothing, because the validation layer wraps non-dispatchable handles and the two logs are in different namespaces — which reads as "this object never went through our destruction path"; and allocation size is the padded VMA block, so it is not an identity either. |
| [gpuav-rewrites-the-usage-it-judges-you-against.md](gpuav-rewrites-the-usage-it-judges-you-against.md) | A validation message names your buffer, your command and your parameters, and is produced by the layer's own instrumentation: GPU-AV adds `STORAGE_BUFFER_BIT` to application buffers, so a correct `addressFlags` of 0 is reported as a VUID-13122 violation. An issue filed from it carried a diagnosis and a fix that would have introduced the opposite violation on every ordinary run. |
| [vulkan-shader-heap-indexing.md](vulkan-shader-heap-indexing.md) | A heap-indexing shader written with the built-in names from memory fails as an undeclared identifier; an index in descriptors cannot address a slot region that starts at a non-multiple offset; a combined sampler passed to a function is rejected as "must appear at point of use"; a material heap offset memoised across a texture reload samples a plausible WRONG texture; two entry shaders sharing one stage body disagreeing about the material arm let a recording thread read the other route's bind decision. |
| [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md) | Every Vulkan draw wrote colour attachment 0 alone, and the forward path only displays attachment 0. |
| [substituted-seams-compound.md](substituted-seams-compound.md) | A decal tenant made three substitutions, each hiding a different live bug; no decal had ever produced a pixel. |
| [testing-architecture.md](testing-architecture.md) §10 | A skipped Vulkan suite USED TO report exactly like a passing one. `VulkanPassSuite` skips with no device, so 58 tests scrolled past as skips above a `[  PASSED  ]` and a stale assertion in it survived #1234 for months; the green PR said nothing about the backend at all. §10's end-of-run banner is the answer — read the row as the failure it now prevents. |
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
| [temporal-reactivity-separation.md](temporal-reactivity-separation.md) | A strand or leaf whose pixel coverage changed is the same instance, primitive, material and depth, so every history test accepts it and the old value is dragged across the LOD step; and a coverage-reactive term WITHOUT a dead band makes stochastic hair sparkle worse than no term at all, while both arms' settled screenshots are identical. |
| [temporal-measurement-control-arms.md](temporal-measurement-control-arms.md) | Three control arms in a row measured nothing and every headline number looked excellent: turning TAA off silently swapped the groom's composition mode so the control shimmered exactly 0, a subject-presence guard that counted lit pixels answered 100% on a frame of empty sky, and a test named for a windy meadow ran on a meadow whose wind-on and wind-off captures differed by 0.012% of pixels, which is the noise floor rather than any displacement at all. |
| [pixel-error-mesh-lod.md](pixel-error-mesh-lod.md) | Projecting through the real view-projection instead of a facing plane passes every value test; only an invariance test under camera direction separates them. |
| [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md) | A vertical wall at the tile edge, and six flat-coloured islands, with every pipeline stage verified correct; "assert the weights vary" passes on the bug. |
| [water-shading-nyquist.md](water-shading-nyquist.md) | Normals derived without the amplitude the displacement carried, for months; a second FFT grid lost 38% of slope RMS while height RMS held. |
| [geometry-lod-measure-the-unreachable-cost.md](geometry-lod-measure-the-unreachable-cost.md) | A tess-control cull rejecting 95% of patches still pays every vertex invocation; a screen-space remap flipped every triangle's winding and the waterline discard deleted the whole surface while eleven hand-built-matrix tests stayed green; a per-patch band-limit tore the surface at every shared vertex. |
| [persistent-world-space-fields.md](persistent-world-space-fields.md) | A wake in an R8 texture renders and follows correctly and never fades, because the decay step rounds to zero. |
| [compute-in-place-vs-ping-pong.md](compute-in-place-vs-ping-pong.md) | Adding a neighbour read to an in-place compute pass races between work groups; it renders, in bands that follow the dispatch order, and looks like a bug in the maths you just wrote. |
| [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md) | Shader and CPU agreed on the function and disagreed on which space its argument was in; four of five evidence cameras pointed away from the boat. |
| [skeletal-deformation-shared-output.md §3](skeletal-deformation-shared-output.md) | A guard added to the one reader that has no callers: the live path kept reading the previous-pose palette raw, the build stayed green, and two review passes went by before it surfaced. Pinned by a text scan over call sites, because a behaviour test passes as soon as one path is correct. |
| [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) | Seven shaders each wrote out linear-blend skinning; the three shadow-path copies had lost both the bone-ID bounds test and the zero-weight guard, so an unweighted vertex collapsed onto the model origin in shadows while rendering at its rest position in colour. Absent from every procedural test mesh, routine in a real import. Pinned by a text-level contract test, not a numeric one. |
| [morph-and-lod-in-the-animated-surface.md](morph-and-lod-in-the-animated-surface.md) | A morphing face ghosted under TAA because the shaders reproject the previous pose through THIS frame's rest surface, so a surface that moved has no expressible velocity at all; and no skinned or morphing mesh could ever change LOD, because four separate places refused to generate, select or draw a level for one. `std::clamp` also cannot reject a NaN weight. |

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
| [render-graph-declaration-config.md](render-graph-declaration-config.md) | Every declaration input was written twice, where it was read and in a hand-assembled cache fingerprint; ten were missing from the second and six execution-only values were in it. |
| [reference-path-tracer.md](reference-path-tracer.md) | A C++ BRDF port against the GLSL it mirrors. |
| [resampled-estimator-measure-convention.md](resampled-estimator-measure-convention.md) | A missing change-of-measure Jacobian in a resampled estimator produces a smooth geometric brightness gradient across a wall, and a double-applied normaliser divides the whole image by the candidate count; both read as "the new tier looks a bit off" and both survive a golden image, a unit test on the Jacobian's own value, and CPU/GPU agreement — because the two implementations were written from the same misunderstanding. |
| [gpu-path-tracer.md](gpu-path-tracer.md) | A GLSL path tracer against the C++ one it mirrors: the same Sobol'-Owen sampler to the bit, the same NEE + MIS structure, the same closure, pinned on the device by a written-down disagreement budget. |
| [baked-lightmap-pipeline.md](baked-lightmap-pipeline.md) | The GI stores' units ledger, and bake-time unwrap parameters mirrored by the runtime resolve. |
| [lightmap-receiver-identity.md](lightmap-receiver-identity.md) | Four walks that must gather the same lightmap receivers; a mismatch renders with no baked GI and no error. |
| [lightmap-receiver-identity.md §VirtualGeometry](lightmap-receiver-identity.md#virtualgeometry-the-uv2-rides-the-vertex-arena-because-no-binding-was-available) | A reserved vertex-pull binding resolves from the VAO, so publishing a buffer there is silently ignored. |
| [component-serializer-codegen.md](component-serializer-codegen.md) | Five exclusion sets, each mirrored by a coverage-test roster. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | The build pipeline and the runtime must agree on an asset layout. |
| [audio-voice-budget.md](audio-voice-budget.md) | One config field costs four edits, one of them silent. |
| [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) | Two build trees writing the same generated files. |
| [build-memory-per-tu.md](build-memory-per-tu.md) | A build-memory number nobody re-measures: five sources disagreed by 3x while `--parallel` and two cgroup caps rested on it. An absolute records path silently zeroes the compiler cache's cross-tree hit rate; a relative one yields one file per subdirectory under Makefiles, and reading only the top-level file ranks the wrong fraction of the build. |
| [build-trees-and-windows-asan.md §4b](build-trees-and-windows-asan.md#4b-live-toolchain-bug-a-throw-from-inside-a-catch-handler-avs-clang-cl--asan) | A throw executed inside a `catch` handler AVs under clang-cl ASan; the catch type and rethrow form are irrelevant. |
| [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md) | A static destructor reading a `thread_local` that `__dyn_tls_dtor` already destroyed; 219 failures, one bug. |
| [concurrent-cmake-configure.md](concurrent-cmake-configure.md) | Two configures sharing one `CMakeFiles/`; the nested `try_compile` that loses reports the error. |
| [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) | Four subsystems hold world-space state outside the rebased set. |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | The packed-quad layout lives in `VoxelQuad.h` and `VoxelQuadUnpack.glsl`, and a mismatch compiles. |
| [destructible-debris.md](destructible-debris.md) | Two unrelated physics layer numberings; `SetCollisionLayer(Debris)` never reaches Jolt's `DEBRIS`. |
| [structural-collapse.md](structural-collapse.md) | Jolt allocates `MotionProperties` at body-creation time, so a `Static` body cannot become `Dynamic` unless it was created with `mAllowDynamicOrKinematic`; an unanchored structure stands until the first break and then vanishes entirely. |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) | Four uint packings in C++ and four GLSL files; a wrong bit renders plausible wrong content. |
| [gpu-scene-record-contract.md](gpu-scene-record-contract.md) | Five records in `GPUSceneTypes.h` and `include/GPUScene.glsl`, pinned by `static_assert` and a SPIRV-Cross reflection test over member names, offsets and stride. |
| [gpu-scene-record-contract.md §7](gpu-scene-record-contract.md#7-the-raster-consumer-a-draw-link-resolved-once-per-frame) | Every raster path that has not migrated keeps its own previous-frame transform, and a second scene representation grows one convenient copy at a time. Pinned by `GPUSceneAntiDuplicationRatchetTest`, which requires every duplicating source file to be named in `GPUSceneLegacyAdapters.h` and every name there to still duplicate. |
| [animated-surface-records.md](animated-surface-records.md) | One record carrying two histories with two different derivation rules. Deriving the deformation history from the slot the way the transform history is derived reports continuity across a rejection declared after the frame's advance, so a velocity is measured between two different surfaces and only TAA shows it. Pinned by `GPUSceneAnimated.TheRevisionPairNeverDisagreesWithTheBonePalettes`, which walks the transitions rather than the steady state. |
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
| [groom-curve-import.md](groom-curve-import.md) | Authored groom intent, when an unknown attribute is ignored rather than refused; and byte-identical cooks, to one unstable sort. |
| [groom-strand-visibility.md](groom-strand-visibility.md) | A whole coat, to an alpha cutoff on sub-pixel strands; a coverage comparison, to a stability metric that scores "loses everything, consistently" as stable; every ribbon quad, to a per-corner tangent that makes it a bowtie. |
| [groom-scene-temporal-resolve.md](groom-scene-temporal-resolve.md) | Every coat in an authored scene, to a stochastic mode refused for want of a TAA box nobody ticked; a coat-coverage A/B, to an edit-mode clip that kept walking between the two captures. |
| [groom-surface-binding.md](groom-surface-binding.md) | A coat onto the wrong part of a body, to a silent rebind after a re-triangulation; every strand's direction, to a second frame construction that disagrees about handedness; a cooked binding's determinism, to a nearest-triangle tie broken by grid cell size; a coat-length smear under TAA, to previous positions carried forward across an LOD switch. |
| [groom-fibre-scattering.md](groom-fibre-scattering.md) | A coat's whole azimuthal response, to a quadrature that point-samples the fibre width and bands as the tangent turns; the environment term, to a circle rule that reads exactly zero at a smooth fibre's grazing angles; dark hair's white sheen, to an R lobe tinted by the pigment; the coat's position AND its lighting direction, to a model matrix left in absolute world space under camera-relative rendering. |
| [groom-guide-simulation.md](groom-guide-simulation.md) | A coat that lags its own animal by a whole animation, to a simulated guide the deformer never selected; a coat that splays under motion with a perfect solver, to a guide displacement sampled by index instead of by parameter; one side of an animal standing still, to a guide budget that took a prefix rather than a stride; a coat rubbery only through a frame spike, to an iteration count mistaken for a length guarantee; a coat pushed off the arm, to a body proxy fitted to extrema instead of percentiles. |
| [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md) | A coloured coat, darkened twice, to a kappa derived from the pigment; a coat's whole shadow, to a reference footprint narrower than the strand spacing, which reports a confident ZERO; a bake-off's conclusion, to a rejected candidate measured four times past its own optimum; the shadow itself, to a resolution raised past two strand spacings, where finer is measurably worse. |
| [groom-representation-lod.md](groom-representation-lod.md) | Four fifths of a coat's density, to a foliage square-root compensation borrowed for band geometry; a coat's whole card tier, to root UVs past the clump-cell clamp, which puts the animal in one cell and still cooks; a coat's binding and its motion past the hand-over, to a LOD level with no map back to the base curve; a hand-over that does nothing, to a cache key that cannot tell a card from a strand; a hysteresis halved, to state kept in a pass that runs once per camera. |
| [groom-ray-traced-proxies.md](groom-ray-traced-proxies.md) | A coat to black, to a groom made a ray-traced-shadow receiver as well as a caster, which shadows it by a second copy of itself; a coat's ray-traced shadow frozen at the pose it was first seen in, to a proxy buffer refilled in place under a build-once BLAS; one flank of an animal missing from every shadow, to a ribbon perpendicular taken from a fixed axis; the whole acceleration structure, to a degenerate segment normalised into a NaN vertex that no validation layer reports; a coat thinner in ray space than on screen, to a proxy that applied the thinning compensation and not the groom's unit-scale lever. |
| [coat-authoring-and-per-role-budgets.md](coat-authoring-and-per-role-budgets.md) | An animal's silhouette, to one global stride that thins the sparse guard hairs and the dense undercoat by the same fraction; a coat's whole role table, to a cook that copies field by field and was never told about it; a saturated tint, to a 24-bit payload bit-cast into a denormal that the vertex pipeline flushes to zero; a "not uniform" claim, to an absolute variance that FALLS when a multiplicative tint darkens the coat, and to a pixel set that the A/B itself moved. |
| [mcp-protocol-eras.md](mcp-protocol-eras.md) | Event pushes, when the notification carrier was swapped instead of run beside the old one. |
| [steamworks-platform-integration.md](steamworks-platform-integration.md) | The whole feature, when the SDK path is one level too high; the build succeeds with Steam off. |
| [configure-time-variable-visibility.md](configure-time-variable-visibility.md) | A DLL copy step for the test executable, on the first configure only. |
| [cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md) | A texture, from the second load onward. |
| [shared-atlas-allocator.md](shared-atlas-allocator.md) | A budget claim, when `vector::resize()` or a `= T{}` reset discards a non-RAII handle. |
| [technique-selection-seams.md](technique-selection-seams.md) | The REASON a light did not get the technique it asked for, when the choice is a shader branch: by the time the shader runs it is one uniform, and nothing on the CPU made the decision. |
| [derived-graph-must-outlive-its-source.md](derived-graph-must-outlive-its-source.md) | Every scene, from the asset dependency graph — no serializer ever registered one — so "what references this texture" answered confidently and short, and every edge that WAS registered pointed backwards, which stayed invisible because the reload path walked the wrong map too. |

**The counter-move:** ask what the absence would look like. If nothing would differ, you need a
coverage test, not a unit test.

## 4. Your instrument is lying to you

The check passes for a correct implementation and for a broken one.

| Doc | The instrument that failed |
|---|---|
| [live-verification-noise-floor.md](live-verification-noise-floor.md) | A crop check that a mirrored, wrong position scored better on; read tools that answer 200 with a stale frame from an iconified window. |
| [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md) | A GPU counter that stopped updating is byte-identical to one that is constant. |
| [automation-build-invocation.md](automation-build-invocation.md) | A build's exit code is 0 three different ways without anything having been built — the lock's stand-down, a no-op incremental, and a build that never started next to last week's binary. |
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
| [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md) | A healthy restore of a current entry that still hits 32 %, and `--show-stats` rounds away the number that would explain it — `Cache size 1 GiB / Max cache size 1 GiB` printed both for a cache 20 % over its cap and for one with 259 MiB spare. Two different causes, one symptom: too small a cap, or a key that does not cover the SDK that changed. |
| [self-hosted-ccache-slot-multiplier.md](self-hosted-ccache-slot-multiplier.md) | A cumulative `76.40 %` hit rate over a cache pinned at `30.0 / 30.0` with 5,884 cleanups, while every job recompiled half the tree. The rate is a lifetime counter shared by every job on the box, so it averages the warm era with the thrash; and half the misses were structural, two runner slots each caching the same object under its own `_work` path. |
| [cache-entry-version-is-the-path-string.md](cache-entry-version-is-the-path-string.md) | The vcpkg key matched exactly and the entry was still invisible: restore and save spelled the same directory two ways, so every entry ever written had `last_accessed_at == created_at`. |
| [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md) | The build step takes compile time on a shader-only change while the cache directory is warm. A PCH-using compile is uncacheable without `CCACHE_SLOPPINESS`; a per-commit macro on every TU misses every run. |

**The counter-move:** measure the noise floor first, and construct a case the instrument must fail
on before trusting a case it passes.

## 5. Ordering and lifetime

The logic is right; when it runs, or how long it lives, is wrong.

| Doc | The ordering or lifetime rule |
|---|---|
| [script-structural-command-safe-point.md](script-structural-command-safe-point.md) | Never mutate the registry mid-iteration. |
| [visual-script-vm.md](visual-script-vm.md) | A loop node that forgets to charge its own iteration hangs the frame, and one that keeps its index on the C++ stack across a `Trigger` restarts it. |
| [runtime-scene-switching.md](runtime-scene-switching.md) | Five ordering rules for a swap that destroys the thing being iterated. |
| [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) | Input before the physics kick, camera last. |
| [two-phase-occlusion-culling.md](two-phase-occlusion-culling.md) | Pass order decides who still sees previous-frame depth. |
| [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) | Clearing the LRU bit one step early evicts the whole cache every frame; a perspective face cannot be culled like an ortho level (§8). |
| [virtual-geometry-into-a-second-shadow-technique.md](virtual-geometry-into-a-second-shadow-technique.md) | A whole caster family loses its shadow when a second technique is switched on and nobody routed it there; a shared parameter block's zero value must not mean "on". |
| [terrain-virtual-texturing.md](terrain-virtual-texturing.md) | Touch a priority-ordered LRU in reverse (§5); coarse-to-fine fill is one dispatch per level with barriers (§3a). |
| [async-fault-in-under-a-residency-cache.md](async-fault-in-under-a-residency-cache.md) | Allocating the slot before the asynchronous read completes evicts a resident entry to hold nothing; every counter stays correct and only the picture gets coarser. |
| [render-pass-published-state.md](render-pass-published-state.md) | Publish last, restore deliberately. |
| [transparent-draw-order.md](transparent-draw-order.md) | An inverted depth in the LEAST significant field orders blended draws only within one shader+material bucket; outside it they composited by shader ID and then material ID, never by depth, and the wrong blend still looks like a blend. |
| [registries-must-outlive-their-registrants.md](registries-must-outlive-their-registrants.md) | A lazily-created registry is destroyed BEFORE the namespace-scope statics whose destructors unregister from it. |
| [cluster-lod-simplification.md](cluster-lod-simplification.md) | A lock must outlive the level that created it (§1); an `isfinite` test accepts the `FLT_MAX` terminal marker and silently selects an empty cut (§5b). |
| [deforming-geometry-conservative-bounds.md](deforming-geometry-conservative-bounds.md) | A rest-pose bound stops containing geometry that deforms, so the cull drops clusters that are on screen; and per-node deformed LOD spheres stop nesting, so the cut cracks along a seam that opens and closes as the character moves. |
| [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md) | A read from a pooled resource whose lifetime already ended. |
| [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md) | TOCTOU between a decrement and a re-read. |
| [non-recursive-lock-self-locking-helper.md](non-recursive-lock-self-locking-helper.md) | A locked scope calling a sibling that locks the same non-recursive mutex; unlock the callee, don't move the caller. |
| [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md) | Promoting a per-tick local to persistent state. |
| [bump-allocator-rollover-padding.md](bump-allocator-rollover-padding.md) | A bump allocator sized its new block with padding computed for the old base, then recomputed padding for the new base with no final bounds check; the cursor advanced out of the block. |
| [parallelizable-mover-systems.md](parallelizable-mover-systems.md) | Split a system at its write boundary. |
| [gl-clear-program-revalidation.md](gl-clear-program-revalidation.md) | What is bound when you clear. |
| [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) | A CPU write between two recorded draws is last-write-wins on Vulkan. |
| [vulkan-large-vertex-stream-command-ordered.md](vulkan-large-vertex-stream-command-ordered.md) | Every animated coat on Vulkan, to a per-frame snapshot 8-14x the size of the arena slot it had to fit in. |
| [vulkan-sampler-array-bindings.md](vulkan-sampler-array-bindings.md) | A sampler array resolved element `i` by heap-slot adjacency, so a batch rendered correctly only while its textures happened to get consecutive slots; the first non-adjacent slot made every element but `[0]` sample an unrelated image, with correct content, a correct descriptor and no validation error. |
| [vulkan-parallel-recording.md](vulkan-parallel-recording.md) | Two parallel items wrote one UBO object, or transitioned one subresource; the merge reports the second, the first renders the wrong cascade. |
| [command-packet-lifecycle.md](command-packet-lifecycle.md) | A pass edited the packets of a bucket it had already replayed. The write was sequential, so the frame was right; the same edit on a bucket replayed by workers is a data race nothing reported. |
| [vulkan-parallel-graph-recording.md](vulkan-parallel-graph-recording.md) | Prepared passes never form a production group, or disabled members make a valid group decline. |
| [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) | A per-instance BLAS loop builds one structure twice; a saturated GPU Scene generation makes a dead record look live. |
| [deformed-surfaces-in-acceleration-structures.md](deformed-surfaces-in-acceleration-structures.md) | An animated mesh whose records became stageable is built into a rest-pose BLAS and traced T-posed: well-formed record, successful build, legal API usage, healthy counters, and the only evidence is the picture. A pose change also moves no field of the geometry record, so a fingerprint-driven refit policy either refits every frame or never. |
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
| [render-graph-setup-declaration-gates.md](render-graph-setup-declaration-gates.md) | Five passes declare nothing when they have no work; one frame rendered with the bucket empty culled the pass, and 1310 generated, culled and submitted plant instances were never drawn. |

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
