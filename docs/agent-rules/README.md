# agent-rules — the index

Two kinds of document live here:

- **Postmortems**: one real failure each, written so the next person doesn't repeat it.
- **Reference guides**: the `notes-*.md` family, accumulated per-subsystem gotchas.

This file indexes them by subsystem: use it when you know what you are touching. When you know what
you are doing but not yet what can go wrong, use the by-failure-mode index in
[failure-modes.md](failure-modes.md); its archetypes cut across subsystems.

Each entry is one sentence stating the rule. The story that taught it is inside the file.

## Code and review standards

- [cpp-coding-quality.md](cpp-coding-quality.md): the coding rules, including float comparison, `auto`, IWYU, and the defaulted `operator==` MSVC quirk.
- [glsl-shaders.md](glsl-shaders.md): the SPIR-V rules a shader must follow to compile: no bare uniforms, UBO bindings, MRT outputs.
- [gl-shader-route.md](gl-shader-route.md): an OpenGL program reaches the driver as SPIR-V unless its shader names `OLO_GL_GLSL_ROUTE`.
- [raw-bindless-route-driver-front-end.md](raw-bindless-route-driver-front-end.md): a shader on the OpenGL bindless route is compiled by the driver's GLSL front end.
- [technique-selection-seams.md](technique-selection-seams.md): when a subsystem grows a second way of computing the same number, the choice is a value carrying the reason it is not what was asked for, not another shader `if`.
- [engine-owned-containers.md](engine-owned-containers.md): new engine-owned sequence and string data is `TArray` / `FString`; the binding surface and every map keep `std::`.
- [sonarqube-review-alignment.md](sonarqube-review-alignment.md): read before `/code-review` so local findings match the cloud profile.

## Testing and verification

- [testing-architecture.md](testing-architecture.md): which renderer layer or Functional axis a new test belongs to, and the registration contract.
- [../testing.md](../testing.md): why we test what we test; value heuristic, anti-patterns, retirement criteria.
- [vulkan-software-driver-ci.md](vulkan-software-driver-ci.md): check a software Vulkan driver against the ADR 0010 extension contract.
- [lavapipe-ray-query-suites.md](lavapipe-ray-query-suites.md): the L7 ray-query suites run nightly on lavapipe behind a gate that fails when any case did not execute.
- [vulkan-support-matrix.md](vulkan-support-matrix.md): run `OloEngine-Tests --olo-vulkan-capability-report` before claiming a machine can or cannot run a Vulkan configuration.
- [renderer-pr-gpu-tests-run-after-merge.md](renderer-pr-gpu-tests-run-after-merge.md): PR CI has no GPU, so run the evidence, CrossPath and Vulkan device suites a renderer change reaches before the PR.
- [substituted-seams-compound.md](substituted-seams-compound.md): every substitution a test makes is a seam it stops testing, and they compound.
- [no-silent-fallbacks.md](no-silent-fallbacks.md): a path that cannot do what it was asked says so loudly and countably.
- [renderer-memory-accounting.md](renderer-memory-accounting.md): book each GPU allocation once as physical backing, keep GPU and CPU bytes apart.
- [measure-in-the-frame-the-pixels-came-from.md](measure-in-the-frame-the-pixels-came-from.md): unproject a readback through the camera its frame was drawn with.
- [completed-frame-perf-windows.md](completed-frame-perf-windows.md): select camera history only after verified renderer progress, and preserve its generation.
- [measurement-validity-and-sentinels.md](measurement-validity-and-sentinels.md): a measured number travels with a status saying whether it IS a measurement.
- [vulkan-query-generations.md](vulkan-query-generations.md): prove completion of the submission that reset and wrote a reused query before trusting its availability or value.
- [planar-reflection-ao-restoration.md](planar-reflection-ao-restoration.md): republish main-view AO/depth bindings after mirror replay; restoring the camera does not restore textures.
- [reference-path-tracer.md](reference-path-tracer.md): the ground-truth oracle for "is it correct", where a golden can only say "did it change".
- [visual-quality-criteria.md](visual-quality-criteria.md): a criterion containing a judgement word such as *convincing* is settled by the frame, not by a proxy; enlarge before judging.
- [resampled-estimator-measure-convention.md](resampled-estimator-measure-convention.md): write a resampled estimator's measure convention into the header that owns the sample.
- [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md): measure the noise floor and audit a recording before baking a per-vendor baseline.
- [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md): give a visual-test scene a ground plane, then look at the PNG.
- [live-verification-noise-floor.md](live-verification-noise-floor.md): measure frame-to-frame noise before attributing a pixel change, and confirm the editor is drawing at all.
- [verification-workload-attribution.md](verification-workload-attribution.md): use Release with symbols for live verification, preserve the full workload, and separate CPU overhead from GPU shadow cost.
- [procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md): a generator fix and its golden rebake ship in the same PR.
- [generated-evidence-stays-out-of-git.md](generated-evidence-stays-out-of-git.md): commit what a test reads, never what it writes; evidence output stays in its ignored directory.
- [timed-wait-test-assertions.md](timed-wait-test-assertions.md): measure timed waits in microseconds and assert one-sided.
- [thread-local-lifetime-at-exit.md](thread-local-lifetime-at-exit.md): keep the "is it still alive?" signal in a trivially destructible `thread_local`; a destroyed one is not readable.
- [shared-temp-dir-test-isolation.md](shared-temp-dir-test-isolation.md): use `TestTempDir.h`, never a fixed temp path.
- [renderer-state-machine-harness.md](renderer-state-machine-harness.md): a new renderer cache, lever or execution mode joins the state-machine manifest as an operation or an equivalence pair.
- [foliage-regeneration-is-async-once-workers-start.md](foliage-regeneration-is-async-once-workers-start.md): after `m_NeedsRebuild`, let `RunFrames` / `RunEditorFrames` finish the regeneration or wait on `IsRebuilding()`.
- [cross-test-renderer-state.md](cross-test-renderer-state.md): never shut down a process-wide singleton you did not start, and leave the renderer configuration as you found it.
- [test-runtime-camera-defaults-to-orthographic.md](test-runtime-camera-defaults-to-orthographic.md): give a fixture's `CameraComponent` a projection; the default is orthographic and culls a subject a few units away.
- [world-anchored-renderer-state-in-tests.md](world-anchored-renderer-state-in-tests.md): a fixture that builds a `Scene` per test must also reset the process-static renderer state scene load resets.
- [fixture-assets-outlive-the-case.md](fixture-assets-outlive-the-case.md): a renderer test case removes the memory-only assets it added, and the files it imported, in `TearDown`.
- [ab-diff-peak-is-not-a-location.md](ab-diff-peak-is-not-a-location.md): place a measurement box by the highest-mean window of an A/B difference, never by its brightest pixel.
- [cross-path-term-measurement-traps.md](cross-path-term-measurement-traps.md): to separate a lighting term by switching its source, assert every toggled estimator engaged and read each state from the target it wrote.
- [motion-claims-need-runtime-frames.md](motion-claims-need-runtime-frames.md): measure a claim about a coat in motion on runtime frames, through the runtime camera, and check they were alive.

## Build and dependencies

- [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md): never build msvc and clangcl trees together; caches, link bounds, memory, the local ASan recipe.
- [worktree-shared-build-outputs.md](worktree-shared-build-outputs.md): write archives and generated headers under the build tree, never the source tree.
- [build-memory-per-tu.md](build-memory-per-tu.md): set `--parallel` and every memory cap from the published per-TU peak-RSS ranking, not from a remembered number.
- [windows-codeview-links.md](windows-codeview-links.md): Windows link memory follows link input, so cut the input (USD via `dev-cached-core`, ThinLTO jobs); GHASH stays off.
- [windows-link-measurements.md](windows-link-measurements.md): the dated full-input link tables (GHASH, archive repetition, split-binary floor, ThinLTO, worker limits) behind the Windows link guide.
- [ci-oom-read-the-kernel-report.md](ci-oom-read-the-kernel-report.md): for a CI OOM kill, read the kernel's OOM report before blaming a neighbour.
- [concurrent-cmake-configure.md](concurrent-cmake-configure.md): one configure at a time per build tree; the error blames your CMakeLists.txt instead.
- [static-archive-4gib-ceiling.md](static-archive-4gib-ceiling.md): a .lib cannot exceed 4 GiB, and `LNK1248` under-reports the overshoot.
- [vcpkg-dependency-management.md](vcpkg-dependency-management.md): read before adding, bumping or removing a dependency; the CRT triplet mismatch is heap corruption.
- [configure-time-variable-visibility.md](configure-time-variable-visibility.md): a CMake variable must be set before the `add_subdirectory()` that reads it.
- [asset-import-usd-alembic.md](asset-import-usd-alembic.md): the importer registry seam, and vendoring OpenUSD / Alembic / MaterialX statically.
- [openusd-build-and-link.md](openusd-build-and-link.md): OpenUSD builds against vcpkg's oneTBB, and a File API guard fails any link with a second one.
- [asset-import-openvdb-volumetric.md](asset-import-openvdb-volumetric.md): keep OpenVDB editor-only, and extend every exhaustive `switch`.
- [groom-curve-import.md](groom-curve-import.md): polygon Alembic import is not curve support; reject malformed curve data instead of clamping it.
- [groom-strand-visibility.md](groom-strand-visibility.md): real hair is sub-pixel at every practical framing, so widen a strand to one pixel and pay for it in alpha.
- [velocity-convention.md](velocity-convention.md): velocity is unjittered at every writer, so static content writes zero, and the resolves read SceneVelocity.
- [animation-clock-survives-unrendered-ticks.md](animation-clock-survives-unrendered-ticks.md): never reset a previous-frame time on a tick that does not render.
- [groom-scene-temporal-resolve.md](groom-scene-temporal-resolve.md): a scene holding a stochastic groom requests engine TAA every frame instead of storing it.
- [mesh-cache-must-return-the-surface-it-was-given.md](mesh-cache-must-return-the-surface-it-was-given.md): a cold import, a warm `.omesh` load and a pack load must give the same index buffer corner for corner.
- [a-bound-coat-rides-its-body.md](a-bound-coat-rides-its-body.md): parent a bound groom's entity to its binding's target.
- [groom-surface-binding.md](groom-surface-binding.md): a binding addresses triangles by index, so a topology mismatch refuses instead of rebinding.
- [groom-runtime-scalp-clearance.md](groom-runtime-scalp-clearance.md): inspect scalp-following curves with simulation enabled through the full animation.
- [groom-fibre-scattering.md](groom-fibre-scattering.md): a fibre is not a surface, so the per-light factor is the strand's projected width and never N dot L.
- [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md): tau is geometric and colourless, so the pigment is not applied twice.
- [groom-coat-body-in-the-volume.md](groom-coat-body-in-the-volume.md): the body a coat grows on has its own RGBA8 texture on the coat's grid, counted exactly where no shadow map answered for the body at the strand.
- [caster-bounds-come-from-the-geometry.md](caster-bounds-come-from-the-geometry.md): compute a one-sided bound from the actual geometry or a derivation that covers it, never from a scaled sample or a related object's bound.
- [groom-dual-scattering.md](groom-dual-scattering.md): a crossed fibre forwards light in its own colour rather than destroying it, so shade the forwarded part.
- [groom-coat-rest-bake.md](groom-coat-rest-bake.md): a coat that opts in bakes its self-shadow volume once, at rest, and each fragment looks it up at its own bind-pose point.
- [groom-strand-depth-prepass.md](groom-strand-depth-prepass.md): draw a coat twice with one program, depth first and shading at depth EQUAL, so a dense coat is shaded once per pixel.
- [creature-eye-region.md](creature-eye-region.md): a creature with separate eye globes needs the face's skin to close just outside the lids' almond all the way round.
- [every-specular-lobe-takes-the-pixel-filter.md](every-specular-lobe-takes-the-pixel-filter.md): widen a coat's GGX lobe by the same screen-space normal spread as the base, on every path, or a delta light's glint is point-sampled into a firefly.
- [groom-deformed-coat-self-shadowing.md](groom-deformed-coat-self-shadowing.md): bake a bound coat from the strands the pass draws, rebake it when they drift past half a voxel.
- [groom-guide-simulation.md](groom-guide-simulation.md): enforce a strand's length by projection rather than by iteration.
- [groom-external-colliders-and-morph-continuity.md](groom-external-colliders-and-morph-continuity.md): a hand or toy (an external collider) pushes fur out to its full radius while the body proxy holds fur only to the groom's depth; the solver damps, springs and rubs the coat relative to the moving body, holds a short hair to its groom, and lifts the coat over a body capsule instead of sweeping it; a morph change rejects motion vectors, never the simulation's history.
- [groom-guide-stand-ins.md](groom-guide-stand-ins.md): take a stand-in's direction from the linear blend of its guides and its length from their mean length where they agree.
- [coat-authoring-and-per-role-budgets.md](coat-authoring-and-per-role-budgets.md): spend a groom's strand budget per coat role, weighted, or a budget cut takes the animal's outline first.
- [groom-representation-lod.md](groom-representation-lod.md): compensate a thinned coat's width by 1/k and not 1/sqrt(k).
- [groom-lod-cross-fade-and-detail-bias.md](groom-lod-cross-fade-and-detail-bias.md): hide a LOD hand-over, and a visibility-budget halving, by drawing both tiers through complementary masks from one per-pixel hash in every pass that rasterises a tier, shadow casters included.
- [rt-scene-follows-the-raster-caster-set.md](rt-scene-follows-the-raster-caster-set.md): a producer that stages TLAS geometry gives what the raster tier does not cast the no-shadow-cast mask.
- [rt-vegetation-budget-charged-where-enforced.md](rt-vegetation-budget-charged-where-enforced.md): a producer charges every limit its backend enforces by dropping work before it queues.
- [rt-quality-pressure-plans-a-complete-tier.md](rt-quality-pressure-plans-a-complete-tier.md): when a ray-tracing producer meets a limit, plan a complete cheaper tier, a bounded hold or an explicit counted fallback before publishing.
- [groom-ray-traced-proxies.md](groom-ray-traced-proxies.md): a groom proxy occludes other receivers and never its own coat.
- [groom-card-coverage.md](groom-card-coverage.md): cook a card as wide as its members cover, never their sum.
- [groom-coat-colour.md](groom-coat-colour.md): a groom's root UVs are in the engine's runtime texture convention (v up).
- [groom-coat-bake-cost.md](groom-coat-bake-cost.md): split a bake's time by stage before cutting it.
- [groom-gpu-strand-deformation.md](groom-gpu-strand-deformation.md): build a bound coat's stream once in each root's bind frame and move it in the vertex shader, keeping the CPU rebuild as a bit-exact reference.
- [groom-scene-lighting-and-wetness.md](groom-scene-lighting-and-wetness.md): light a coat from the rung of the ambient ladder its body takes and from the Forward+ cluster lists where they are live.
- [multi-animal-scheduling-budgets.md](multi-animal-scheduling-budgets.md): share one frame's work out among a POPULATION of animals, in calibrated units and never in a clock reading.
- [groom-into-the-shadow-techniques.md](groom-into-the-shadow-techniques.md): a caster family whose geometry is built by a later pass borrows it through that pass.
- [groom-casters-in-vsm-local-layers.md](groom-casters-in-vsm-local-layers.md): a groom reaches a lamp's VSM layers one draw per layer its box reaches.
- [incremental-build-odr-staleness.md](incremental-build-odr-staleness.md): when a correct fix makes no sense live, suspect a stale incremental object before the code.
- [pch-masked-missing-includes.md](pch-masked-missing-includes.md): a header must include what it uses even when the build is green; the PCH hides the omission on Windows.
- [ci-cache-that-looks-alive.md](ci-cache-that-looks-alive.md): a CI cache that restores is not one that works.
- [actions-cache-budget.md](actions-cache-budget.md): the Actions cache store is a fixed budget every key must fit in.
- [sccache-cap-vs-object-set.md](sccache-cap-vs-object-set.md): set a compiler cache's size cap above the object set it has to hold, measured with `du`.
- [windows-sccache-warm-reuse.md](windows-sccache-warm-reuse.md): require native compiler statistics after a complete warm restore.
- [self-hosted-ccache-slot-multiplier.md](self-hosted-ccache-slot-multiplier.md): size a self-hosted compiler cache by configurations x the runner slots that can build them.
- [cache-entry-version-is-the-path-string.md](cache-entry-version-is-the-path-string.md): a cache's restore and its save must be handed the same path string, character for character.
- [compiler-cache-uncacheable-compiles.md](compiler-cache-uncacheable-compiles.md): print the cache statistics after every CI build and read the uncacheable line.
- [ccache-pch-result-key-collision.md](ccache-pch-result-key-collision.md): never let ccache's `base_dir` share a precompiled header across checkout roots; run the PCH-producing compile without it.
- [shader-pack-bake.md](shader-pack-bake.md): the CI-baked `.osp` pack, its content-hash invalidation, and why a fresh worktree does not fetch it.
- [steamworks-platform-integration.md](steamworks-platform-integration.md): the SDK is developer-supplied, CI builds a stub, and exactly one TU may include a Valve header.

## Renderer

- [multi-mode-block-format-encoders.md](multi-mode-block-format-encoders.md): validate a block format with several block layouts one mode at a time against the reference decoder's own read order.
- [amd-mesa-shader-compile-blowup.md](amd-mesa-shader-compile-blowup.md): write a per-subset shader step as `subset 0; if (subsets > 1) subset 1` with constant array indices, never a `for` loop over indexed arrays.
- [incomplete-texture-samples-as-zero.md](incomplete-texture-samples-as-zero.md): set a texture's sampler state at creation; an incomplete texture reads zero on AMD and fine on NVIDIA.
- [alpha-cutout-mip-chains.md](alpha-cutout-mip-chains.md): build an alpha cutout's mips by matching each level's alpha histogram to level 0's.
- [vulkan-block-compressed-upload.md](vulkan-block-compressed-upload.md): upload a cooked BCn chain level by level with whole-level texel extents, and refuse an unsupported format by name.
- [std-distributions-are-not-portable.md](std-distributions-are-not-portable.md): seed procedural content with your own transform over mt19937.
- [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md): the OpenGL boundary leaks through the include graph, not a `glXxx(` grep.
- [gl-write-once-buffers-are-immutable-storage.md](gl-write-once-buffers-are-immutable-storage.md): create a large fill-once buffer with `VertexBuffer::CreateImmutable` / `IndexBuffer::CreateImmutable`.
- [framebuffer-bind-owns-the-viewport.md](framebuffer-bind-owns-the-viewport.md): `Framebuffer::Bind()` sets the viewport to the target's active size.
- [oit-blend-state-and-identity-lanes.md](oit-blend-state-and-identity-lanes.md): a pass drawing into the WB-OIT targets re-states its per-attachment blend after anything that can change it.
- [transparent-draw-order.md](transparent-draw-order.md): a conventional alpha-blended draw sorts depth-first as a per-render-mode bit layout.
- [delayed-read-test-must-prove-the-new-version.md](delayed-read-test-must-prove-the-new-version.md): a delayed-read device test reads the NEW buffer version back and requires the new bytes before it checks the old version survived.
- [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md): a CPU buffer write between two recorded draws is last-write-wins on Vulkan.
- [vulkan-large-vertex-stream-command-ordered.md](vulkan-large-vertex-stream-command-ordered.md): a vertex stream whose writes exceed half an arena slot is written by transfers recorded in command order instead of arena snapshots.
- [discard-is-not-a-bounds-check.md](discard-is-not-a-bounds-check.md): in a fragment shader, bound a GPU-driven index by clamping it, because `discard` does not stop the dependent load.
- [vulkan-sampler-array-bindings.md](vulkan-sampler-array-bindings.md): map an array binding with the INDIRECT_INDEX_ARRAY source and write one heap index per element.
- [vulkan-parallel-recording.md](vulkan-parallel-recording.md): a pass forks with `RenderCommand::RecordParallel` and gives every item its own resource objects.
- [command-packet-lifecycle.md](command-packet-lifecycle.md): write a command packet, bucket or `FrameDataBuffer` range only before its first replay.
- [vulkan-parallel-graph-recording.md](vulkan-parallel-graph-recording.md): schedule ready prepared passes before compiling resource lifetimes, and publish shared state only after joining.
- [vulkan-parallel-pass-audit.md](vulkan-parallel-pass-audit.md): prepare resources and freeze shared inputs before a pass records in parallel.
- [vulkan-async-compute-queue.md](vulkan-async-compute-queue.md): a resource crossing between the graphics and async compute queues needs a matched release/acquire ownership pair, not just a semaphore.
- [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md): a BLAS is per geometry and opacity is per instance.
- [deformed-surfaces-in-acceleration-structures.md](deformed-surfaces-in-acceleration-structures.md): animated surfaces need a producer that writes the pose to a buffer before a BLAS can hold it.
- [vulkan-extension-adoption-claims.md](vulkan-extension-adoption-claims.md): check an extension's claims against the pinned SDK's `validusage.json` and `vk.xml` before building on them.
- [vulkan-descriptor-heap-rebind-after-execute-commands.md](vulkan-descriptor-heap-rebind-after-execute-commands.md): bind the descriptor heaps from a recording's first command and again right after every `vkCmdExecuteCommands`.
- [vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md): a device-fault address means nothing alone, so turn on device-fault, NV checkpoints, the address binding report and Aftermath together.
- [vulkan-parallel-cascade-recording-fault.md](vulkan-parallel-cascade-recording-fault.md): bisect a Vulkan parallel-recording fault by forcing one region inline at a time.
- [shared-uniform-binding-bind-before-dispatch.md](shared-uniform-binding-bind-before-dispatch.md): call `Bind()` on a uniform buffer before every dispatch that reads it when its binding point is shared.
- [gpuav-rewrites-the-usage-it-judges-you-against.md](gpuav-rewrites-the-usage-it-judges-you-against.md): attribute a validation message to a layer configuration before changing engine code.
- [vulkan-shader-heap-indexing.md](vulkan-shader-heap-indexing.md): only a shader with no OpenGL twin may index the descriptor heap for any texture.
- [gpu-path-tracer.md](gpu-path-tracer.md): the GPU reference path tracer mirrors the CPU one term by term, so a change to either tracer's transport is a change to both.
- [gl-global-setter-resets-indexed-state.md](gl-global-setter-resets-indexed-state.md): `glColorMask` and `glEnable(GL_BLEND)` are indexed calls for every draw buffer; never port one as a fallback.
- [a-pass-opens-its-own-colour-mask.md](a-pass-opens-its-own-colour-mask.md): a pass sets the colour write mask of every attachment it writes right before its draw.
- [lazy-static-release-ownership.md](lazy-static-release-ownership.md): release a shared lazy static from an unconditional teardown, not from `Renderer3D::Shutdown`.
- [registries-must-outlive-their-registrants.md](registries-must-outlive-their-registrants.md): a process-wide registry a destructor unregisters from must be a deliberately leaked singleton, never a plain static.
- [gpu-debug-draws.md](gpu-debug-draws.md): any shader can draw a primitive into the viewport; read the overflow protocol before concluding "it drew nothing".
- [shader-tuning-constants-need-a-unit.md](shader-tuning-constants-need-a-unit.md): give every authored render constant a unit that does not change with camera range, cascade or resolution.
- [observer-camera.md](observer-camera.md): the frozen culling camera decides what is drawn, never how it looks.
- [gpu-scan-compaction.md](gpu-scan-compaction.md): no early return in front of a work-group scan; test compaction order, not sets.
- [variable-rate-compute-shading.md](variable-rate-compute-shading.md): measure departure from a plane, not depth range, and read the heatmap first.
- [gpu-readback-stats-channel.md](gpu-readback-stats-channel.md): publish GPU counters by name without stalling; the buffer-binding namespace is full.
- [ssbo-binding-cap-is-80-on-mesa.md](ssbo-binding-cap-is-80-on-mesa.md): every `SSBO_*` binding stays below 80, because Mesa exposes 80 storage-buffer binding points.
- [gpu-scene-record-contract.md](gpu-scene-record-contract.md): a GPU-scene record changes in C++ and GLSL in one commit, and only an incompatible edit or a removal advances its generation.
- [gpu-scene-record-contract.md §7](gpu-scene-record-contract-details.md#7-the-raster-consumer-a-draw-link-resolved-once-per-frame): a migrated draw carries a link to its record, resolved once after the commit.
- [animated-surface-records.md](animated-surface-records.md): a record's deformation history is copied verbatim from the producer that owns the palettes, never derived from the slot.
- [stochastic-sampling-and-temporal-resolve.md](stochastic-sampling-and-temporal-resolve.md): blue noise is a claim about the error spectrum, the VNDF weight fails silently, and a resolve clips, not clamps.
- [temporal-reactivity-separation.md](temporal-reactivity-separation.md): carry history rejection as three separated causes rather than one confidence.
- [temporal-coverage-means-not-samples.md](temporal-coverage-means-not-samples.md): where coverage comes from a stochastic estimator, the temporal coverage term compares the two frames' neighbourhood means, not a pixel against last frame's range.
- [temporal-long-sequence-protocol.md](temporal-long-sequence-protocol.md): judge a temporal estimator from paired replays started by one `ResetFrameSequences(seed)`.
- [temporal-measurement-control-arms.md](temporal-measurement-control-arms.md): a control arm that reads exactly zero is broken rather than clean.
- [screen-space-denoiser-chain.md](screen-space-denoiser-chain.md): a filter that runs at half resolution is guided by a guide buffer at its own resolution.
- [gl-clear-program-revalidation.md](gl-clear-program-revalidation.md): wrap every new clear site in `GLClearProgramGuard`, unbind and restore.
- [render-pass-published-state.md](render-pass-published-state.md): a pass that publishes engine-global bindings runs last and is not wrapped in `GLStateGuard(Restore)`.
- [render-graph-attachment-feedback.md](render-graph-attachment-feedback.md): validate the original accesses against physical attachment identities, and give writable attached depth a snapshot.
- [render-graph-transient-aliasing.md](render-graph-transient-aliasing.md): `WriteNewVersion` renames a physical resource; use the poison and disable levers to find stale reads.
- [render-pipeline-caches.md](render-pipeline-caches.md): process-wide render caches invalidate on every topology reset, not only on a fingerprint change.
- [pso-precache-readiness.md](pso-precache-readiness.md): a pipeline a draw has to create is a counted, classified miss; requests come from the PSO manifest crossed with live shaders, and render-ready means every selected-configuration request is created.
- [render-graph-out-of-band-work.md](render-graph-out-of-band-work.md): anything a pass touches that the graph cannot back is a named out-of-band boundary declared in `Setup()` and noted where it is accessed.
- [render-graph-setup-declaration-gates.md](render-graph-setup-declaration-gates.md): a pass whose `Setup()` declares nothing when it has no work must report that gate in the declaration key, or it stays culled while its bucket fills.
- [render-graph-declaration-config.md](render-graph-declaration-config.md): anything `Setup()` or `PopulateBlackboard` branches on is a `FrameGraphDeclarationConfig` field or an `AppendDeclarationInputs` entry.
- [render-graph-repopulate-forces-setup.md](render-graph-repopulate-forces-setup.md): when the blackboard re-populates, invalidate the build cache before `BuildFrameGraph`, not from inside `PopulateBlackboard`.
- [ddgi-probe-cascades-and-sparsity.md](ddgi-probe-cascades-and-sparsity.md): the DDGI clipmap is toroidal, `%` truncates toward zero, and sparsity fails as "no GI, no error".
- [baked-lightmap-pipeline.md](baked-lightmap-pipeline.md): the GI units ledger, UV2 as a parallel stream, and keying the seam split after the unwrap.
- [lightmap-receiver-identity.md](lightmap-receiver-identity.md): a baked lightmap region is addressed by (entity UUID, sub-key), never by the entity alone.
- [two-phase-occlusion-culling.md](two-phase-occlusion-culling.md): phase 1 tests the previous frame's final pyramid, and pass order decides who sees old depth.
- [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md): keep the four page-cache invariants; a `Setup()` that branches on a runtime toggle is frozen by the fingerprint cache.
- [foliage-vsm-page-caching.md](foliage-vsm-page-caching.md): moving foliage invalidates a VSM clip level only where its texel is finer than the layer's declared displacement.
- [shadow-pass-attribution-and-cull-slots.md](shadow-pass-attribution-and-cull-slots.md): time ShadowPass through its family and caster-class brackets.
- [shadow-receiver-bias-is-texels-of-the-map-sampled.md](shadow-receiver-bias-is-texels-of-the-map-sampled.md): bias a shadow receiver by the light's depth texels and normal metres, converted through the level or mip it samples.
- [virtual-geometry-into-a-second-shadow-technique.md](virtual-geometry-into-a-second-shadow-technique.md): a caster family reaches a shadow technique only if somebody wired it there, and the gap is invisible.
- [cluster-lod-simplification.md](cluster-lod-simplification.md): a terminal group's boundary lock outlives the level that created it, and a terminal group is marked `FLT_MAX`.
- [deforming-geometry-conservative-bounds.md](deforming-geometry-conservative-bounds.md): a bound a hierarchy's invariants rest on grows by one uniform scalar, derived rather than estimated.
- [pixel-error-mesh-lod.md](pixel-error-mesh-lod.md): the LOD plane faces the mesh, not the camera, and the error metric must be a ratio.
- [compute-written-texture-mip-chain.md](compute-written-texture-mip-chain.md): every writer of mip 0 owes the rest of the chain.
- [terrain-gpu-lod-quadtree.md](terrain-gpu-lod-quadtree.md): crack-freedom is a vertex-set property, and picking must not inherit the tessellation gate.
- [terrain-virtual-texturing.md](terrain-virtual-texturing.md): every VT defect is a wrong address; touch LRU in reverse priority, and treat an eviction as an entry, not an absence.
- [async-fault-in-under-a-residency-cache.md](async-fault-in-under-a-residency-cache.md): obtain the payload before you claim the slot, or the cache evicts a live entry for bytes that have not arrived.
- [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md): measure a tile's outermost ring, and never key an auto-material rock rule below the shoreline mask's slope.
- [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md): the packed-quad encoding is mirrored in GLSL, and a merged quad can render plausibly and wrong six ways.
- [camera-relative-rendering.md](camera-relative-rendering.md): every world-space GPU upload is a site, because f32 cancellation shows as jitter and shadow swim.
- [distance-impostor-reflection-probes.md](distance-impostor-reflection-probes.md): one encoding contract is mirrored in three places, and a miss sentinel must not shade from stale sky.
- [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md): separate the three ways impostor cards go missing by reading PNGs from several azimuths.
- [vegetation-asset-import.md](vegetation-asset-import.md): modelled foliage cannot be decimated, so bake its cards out of the scan.
- [foliage-lod-transition-coverage.md](foliage-lod-transition-coverage.md): put a thinning fade band above the keep threshold and compensate the effective keep fraction, not the raw one.
- [foliage-hierarchical-wind.md](../guides/foliage-hierarchical-wind.md): share deformation across raster passes, bound square impostor corners.
- [foliage-interaction-bending.md](../guides/foliage-interaction-bending.md): express a recovery in seconds and integrate the spring analytically, and make an empty influence set contribute exactly zero.
- [procedural-scatter-hash-and-habitat-rules.md](procedural-scatter-hash-and-habitat-rules.md): judge a placement hash by its distinct-offset count, and default every new scatter rule to the multiplicative identity.
- [capture-paths-take-the-projection-seam.md](capture-paths-take-the-projection-seam.md): every matrix a vertex stage feeds to `gl_Position` goes through `RHIProjectionSeam`, a bake's private ortho included.
- [screen-uv-unpack-takes-the-row-flip.md](screen-uv-unpack-takes-the-row-flip.md): a pass that unpacks screen uv into view space builds its constants from `AdjustProjectionForShaderReconstruction`.
- [cpu-texel-rects-take-the-row-flip.md](cpu-texel-rects-take-the-row-flip.md): project a texel rect the CPU computes for a copy, readback or scissor through `AdjustProjectionForShaderReconstruction`.
- [light-path-photometric-parity.md](light-path-photometric-parity.md): the three light evaluators must agree, and a dropped GPU struct field is a dead knob.
- [forward-deferred-parity-measurement.md](forward-deferred-parity-measurement.md): before blaming a shading function for a Forward/Deferred difference, turn off everything the paths apply differently.
- [directional-shadow-ownership.md](directional-shadow-ownership.md): the directional cascades belong to light 0 alone.
- [lighting-signal-contract.md](lighting-signal-contract.md): every lit-surface term has one owner, never two additive estimates.
- [volumetric-cloud-debugging.md](volumetric-cloud-debugging.md): eight causes of a uniform veil, and how to tell "darker" from "directionally darker".
- [water-shading-nyquist.md](water-shading-nyquist.md): a derived normal carries every factor its displacement carries; drop sub-pixel detail rather than filter it.
- [water-displaced-once-and-pinned-captures.md](water-displaced-once-and-pinned-captures.md): if a program has a tess-eval stage the vertex stage must not displace, and an A/B of animated content needs a pinned clock.
- [geometry-lod-measure-the-unreachable-cost.md](geometry-lod-measure-the-unreachable-cost.md): count the invocations paid upstream of a shader-stage cull.
- [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md): the shared thing between a shader and gameplay sampling must be an analytic record, in one agreed space.
- [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md): deform a skinned vertex in one shared include that every pass calls.
- [morph-and-lod-in-the-animated-surface.md](morph-and-lod-in-the-animated-surface.md): apply morph deltas to the rest surface before skinning.
- [per-draw-material-stamp-rides-the-copy.md](per-draw-material-stamp-rides-the-copy.md): a value stamped onto a per-draw `Material` copy must be in the hand-written copy constructor and `operator=`.
- [persistent-world-space-fields.md](persistent-world-space-fields.md): a multiplicative decay is unrepresentable in a normalized-integer texture below a rate threshold.
- [compute-in-place-vs-ping-pong.md](compute-in-place-vs-ping-pong.md): a compute pass may update a field in place only while every invocation reads its own texel.
- [pbf-solver-stability.md](pbf-solver-stability.md): PBF/SPH reference constants assume unit-mass particles.
- [shared-atlas-allocator.md](shared-atlas-allocator.md): the buddy allocator behind the shadow atlas and impostor budget; swap, don't mutate, and beware the non-RAII handle.

## Scene, ECS and serialization

- [scene-registry-source-path.md](scene-registry-source-path.md): resolve scene registry paths against the active project before reading or writing source files.
- [project-optional-paths.md](project-optional-paths.md): preserve empty optional project paths before resolving them against the asset directory.
- [component-serializer-codegen.md](component-serializer-codegen.md): when a component round-trips for free, when to annotate a field, and every generated touch-point's exclusion set.
- [scene-binary-sidecar.md](scene-binary-sidecar.md): the `.scenebin` fast path is generated and hybrid-covered.
- [binary-format-versioning.md](binary-format-versioning.md): every on-disk format reads exactly its current version and rejects any other with a fix message.
- [cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md): a cache must refuse to store a name nothing can resolve; the failure shows on the second load only.
- [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md): `Scene::Copy()` must carry every scene-level settings struct into Play.
- [scene-render-settings.md](scene-render-settings.md): a setting the scene owns and the renderer draws with goes in `SceneRenderSettings` plus both scene emitters, the shared reader and `Scene::Copy`.
- [terrain-voxel-authored-state.md](terrain-voxel-authored-state.md): component copies share the voxel volume and separate worlds detach it; only authored volumes are persisted.
- [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md): four subsystems hold world-space state outside the rebased set, each needing a different fix.
- [asset-degradation-and-constructor-preconditions.md](asset-degradation-and-constructor-preconditions.md): a precondition asserted in a constructor delegates safety to every call site.
- [derived-graph-must-outlive-its-source.md](derived-graph-must-outlive-its-source.md): before answering "what points at X" from an existing graph, enumerate its writers and check the direction of every edge.

## Gameplay, physics and simulation

- [headless-input-window.md](headless-input-window.md): check the headless host role before obtaining a platform input window.
- [force-model-vehicles.md](force-model-vehicles.md): boats and aircraft driven by `AddForce`; every bug here leaves the suite green.
- [jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md): drive a pinned cloth vertex by velocity, never by position.
- [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md): a `CharacterVirtual` is invisible to UUID-keyed body filters, and a follow camera runs last.
- [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md): a valid navmesh silently switches every `NavAgentComponent` onto the crowd follower.
- [parallelizable-mover-systems.md](parallelizable-mover-systems.md): how to move an entity-moving system onto a worker thread, and the two traps in its determinism test.
- [terrain-collision-streaming-sculpt.md](terrain-collision-streaming-sculpt.md): streamed tile bodies live in a second `JoltScene` map, and tile pose must match the draw transform exactly.
- [procedural-skinned-mesh-primitives.md](procedural-skinned-mesh-primitives.md): code-built test meshes need a live GL context and a shared bind-pose origin.
- [destructible-debris.md](destructible-debris.md): pre-authored debris chunks, and two unrelated physics layer numberings.
- [structural-collapse.md](structural-collapse.md): anchors are explicit, and sideways load transfer is charged for so a wall can partially collapse.
- [audio-voice-budget.md](audio-voice-budget.md): admit the voice cap inside `Play()`, because sounds start from six call sites.

## Scripting, networking and tooling

- [script-structural-command-safe-point.md](script-structural-command-safe-point.md): a script binding that changes the registry structurally queues a command, never acts inline.
- [visual-script-vm.md](visual-script-vm.md): `Trigger` queues a branch rather than running one, so a loop node charges its own iteration and keeps it in `NodeState`.
- [runtime-scene-switching.md](runtime-scene-switching.md): the host applies a scene swap after the tick; five ordering rules and the `Project` mount.
- [packaged-game-content-by-path.md](packaged-game-content-by-path.md): a file a scene opens by path ships only if Build Game stages it, so store such paths in `ResolveContentPath`'s spelling.
- [asset-pack-round-trips-through-the-builder.md](asset-pack-round-trips-through-the-builder.md): a test that claims the packed path builds with `AssetPackBuilder`, loads through a `RuntimeAssetManager`.
- [server-authoritative-networking-loop.md](server-authoritative-networking-loop.md): grep for callers of the entry point, not for tests.
- [mcp-setter-based-field-registry.md](mcp-setter-based-field-registry.md): copy-then-swap MCP writes are unsound when `operator=` cannot reproduce a setter's side effects.
- [editor-input-coordinates-and-imgui-viewports.md](editor-input-coordinates-and-imgui-viewports.md): editor window coordinates are physical pixels on Windows, and synthetic input has to name the ImGui viewport it hovers.
- [mcp-protocol-eras.md](mcp-protocol-eras.md): the stateless core is a second transport; adding `server/discover` alone breaks working clients.
- [automation-build-invocation.md](automation-build-invocation.md): a build started from inside the editor goes through `build-lock.ps1` or it does not happen.
- [automation-event-bus.md](automation-event-bus.md): an event carries identities, never a read's content, and only a mutating command publishes its completion.
- [claude-code-mods.md](claude-code-mods.md): an agent-process rule broken more than once is held by a Claude Code mod in `.claude/mods/`, not by another memory note.

## Concurrency and memory

- [intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md): a decrement-then-reread of a refcount is a double-free even with atomics.
- [non-recursive-lock-self-locking-helper.md](non-recursive-lock-self-locking-helper.md): fix the callee that locks internally; don't wrap a self-synchronised member in an outer lock.
- [spinlock-payload-cache-line-separation.md](spinlock-payload-cache-line-separation.md): keep a lock off its payload's cache line.
- [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md): three checks before promoting a per-tick scratch vector to persistent state.
- [retained-scratch-has-an-owner-and-a-ledger.md](retained-scratch-has-an-owner-and-a-ledger.md): retained scratch is passed in by an owner that gives it back and counted once in the memory report.
- [bump-allocator-rollover-padding.md](bump-allocator-rollover-padding.md): a rollover must not reserve padding computed for the block it is leaving; over-align the blocks instead.
- [cancel-a-load-by-removing-its-record.md](cancel-a-load-by-removing-its-record.md): a cancelled async load is dropped by removing its pending record, and abandoned tasks are waited out at shutdown.

## Subsystem notes (`notes-*.md`)

Accumulated per-subsystem gotchas. Skim the relevant one before working in that area.

- [notes-renderer.md](notes-renderer.md): offline capture, GL wrappers, shader bindings, SSAO/SSR/FSR, IBL bakes, GPU timers.
- [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md): the three-part tool split, schemas, consent and undo, `AutomationUndo`, frame capture.
- [notes-core-and-threading.md](notes-core-and-threading.md): yaml-cpp decode, `Ref<T>` constness, the task system, EnTT first-touch, and C++ traps.
- [notes-gameplay-physics-nav.md](notes-gameplay-physics-nav.md): the two Jolt systems, joints, Detour, dialogue guards, the gameplay scheduler.
- [notes-audio-animation-sim.md](notes-audio-animation-sim.md): pose sampling, retargeting, morph targets, the fixed-timestep split, SoundGraph.
- [notes-editor-and-assets.md](notes-editor-and-assets.md): Content Browser, filewatch import, placeholders, texture cook, the ScriptCore build edge.
- [asset-loads-belong-to-the-game-thread.md](asset-loads-belong-to-the-game-thread.md): a worker that loads assets hands each load to the game thread (`Tasks::RunOnGameThreadAndWait`) and releases what owns them there (`Tasks::PostToGameThread`).

## Adding a doc here

The corpus grows about ten files a month, so style and size matter more than completeness.

1. **Write it when the bug is fixed.** The wrong hypotheses you held evaporate within a day.
2. **Title it as the rule.** "Drive a pinned vertex by velocity, not position", not "Cloth notes".
3. **Rule first, story second.** The first sentence says what to do; the failure follows.
   No metaphor where a literal phrase exists.
4. **Lead with what stayed green.** A failure a test caught usually needs no doc; these files exist
   for the failures tests could not see.
5. **Cite the issue number.**
6. **Add two links:** one sentence in this file under its subsystem, and one row in
   [failure-modes.md](failure-modes.md) under the archetype it belongs to. A rule that should load
   when certain files are touched also gets a line in the matching `.claude/rules/*.md`. Nothing
   goes into `CLAUDE.md`.
7. **Keep it under about 10 KB.** Past that it stops being read: keep the rules in `<name>.md`, each linking to its section in a `<name>-details.md` companion that
   holds the stories, measurements and derivations.
