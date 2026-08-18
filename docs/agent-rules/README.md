# agent-rules — index by failure mode

This directory holds two kinds of document:

- **Postmortems** — one real failure each, written so the next person doesn't repeat it. Indexed by
  failure mode below.
- **Reference guides** — the `notes-*.md` family, accumulated per-subsystem gotchas rather than a
  single failure. Listed under *Subsystem notes*.

`CLAUDE.md` → *Companion guides* lists everything **by subsystem** — use that when you know what
you're touching. Use **this** file when you know what you're *doing* but not yet what can go wrong.

The recurring archetypes below cut across subsystems. If your change fits one, read that row's docs
even when they're about a subsystem you're not in — the failure transfers, the code doesn't.

---

## 1. The suite is green and the feature is broken

The dominant archetype in this repo. A change compiles, passes every contract test, produces
plausible output, and is completely wrong. If your work is in one of these areas, a passing test
run is **not** evidence.

| Doc | What stayed green |
|---|---|
| [force-model-vehicles.md](force-model-vehicles.md) | a boat with no thrust still floats; an aircraft that oscillates still has finite positions |
| [jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md) | the cape detaches, jitters or freezes rigid — unit tests pass through all three |
| [foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md) | three separate bugs, each rendering a *plausible* frame, all reading as "impostors missing" |
| [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md) | a bright saturated material renders near-black and the test asserts nothing about it |
| [scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) | settings reset the instant Play starts; `FunctionalTest` never calls `Scene::Copy()`, so headless can't see it |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | two lighting bugs that 4300 green tests missed |
| [component-serializer-codegen.md](component-serializer-codegen.md) | a corrupt drive mode clamped to a *different valid* mode — the car still drove |
| [asset-degradation-and-constructor-preconditions.md](asset-degradation-and-constructor-preconditions.md) | "load the scene, does it crash?" passes while the bug is fully present — the trigger is *resolution*, not loading |
| [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) | a test believed it exercised the manual path; a valid navmesh had silently switched it to the crowd follower |
| [follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) | a steady-state offset check passes with a full one-tick lag present |
| [parallelizable-mover-systems.md](parallelizable-mover-systems.md) | a position check passes on the scheduler tie-break alone, with the dependency edge missing |
| [mcp-protocol-eras.md](mcp-protocol-eras.md) | adding `server/discover` alone keeps every test green — and converts a *working* legacy fallback into a broken modern conversation, because answering it is a client's proof the server is modern |
| [vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) | two scenes render skybox-only, one renders perfectly, zero errors — no tenant interleaved two uploads of one SSBO with draws |
| [gpu-scan-compaction.md](gpu-scan-compaction.md) | a compaction test that sorts both sides passes identically on `atomicAdd` and on the scan meant to replace it — the *set* was never the broken thing |
| [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md) | every glyph in the engine invisible on AMD, with the font loaded, 189 glyphs packed and 852 quads submitted — bake that and the nightly defends a blank UI forever |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | a merged quad with U and V swapped, or width and height transposed, still merges and still draws — five of the six face directions look right |

**The counter-move:** name the observation that *would* have failed. Usually it's a moving target
instead of a static one, an edge instead of a steady state, a second camera angle, or the physical
quantity in pinned units rather than a composited pixel.

## 2. One contract, several mirrors

The same fact written down in more than one place. Nothing enforces agreement, so they drift, and
the drift is silent because each side is individually self-consistent.

| Doc | The mirrors |
|---|---|
| [distance-impostor-reflection-probes.md](distance-impostor-reflection-probes.md) | header ↔ bake ↔ GLSL — pinned by a regex parity test |
| [light-path-photometric-parity.md](light-path-photometric-parity.md) | three light evaluators that can render the same scene |
| [reference-path-tracer.md](reference-path-tracer.md) | a C++ BRDF port against the GLSL it mirrors |
| [component-serializer-codegen.md](component-serializer-codegen.md) | five exclusion sets, plus generator-vs-coverage-test rosters |
| [runtime-scene-switching.md](runtime-scene-switching.md) | the build pipeline and the runtime must agree on an asset layout, or scripts never load |
| [audio-voice-budget.md](audio-voice-budget.md) | adding one config field costs four edits, one of them silent |
| [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) | two build trees writing the same generated files |
| [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) | four subsystems holding world-space state outside the set that gets rebased |
| [binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) | the packed-quad bit layout, face numbering and U/V basis live in `VoxelQuad.h` **and** `include/VoxelQuadUnpack.glsl`; nothing links them and a mismatch compiles |
| [destructible-debris.md](destructible-debris.md) | two unrelated physics "layer" numberings — `SetCollisionLayer(Debris /*7*/)` never reaches Jolt's `ObjectLayers::DEBRIS /*4*/`, so debris silently shoves the player |
| `ShaderBindingLayout.h` ↔ `include/BindlessHeap.glsl` | `HEAP_IMAGE_SLOT_BASE` is *derived* (`= MAX_ENGINE_TEXTURE_SLOTS`) but its GLSL twin `OLO_HEAP_IMAGE_BASE` is a hand-written literal — so **adding any `TEX_*` slot is also a shader edit**. #702 added one, the base moved 66→67, and every bindless storage image read a sampler descriptor through an image declaration (undefined, not blank). The comment there had claimed the two "cannot disagree". Now pinned by `BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout` |

**The counter-move:** a parity test that reads both sides as text, or a generator that makes one
side derived. A comment saying "keep these in sync" is not a mechanism.

## 3. It silently drops something

No crash, no error, no log line. Work or data disappears and the system keeps running.

[binary-greedy-voxel-meshing.md](binary-greedy-voxel-meshing.md) (a stale wall between two chunks,
when a carve does not rebuild the neighbours it uncovered) ·
[component-serializer-codegen.md](component-serializer-codegen.md) (a field, from every save) ·
[scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) (a
settings struct, on entering Play) ·
[light-path-photometric-parity.md](light-path-photometric-parity.md) (an authored light parameter →
a dead knob) · [floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) (world
position, gradually) · [runtime-scene-switching.md](runtime-scene-switching.md) (every script, in
shipped games only) · [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) (a
component's teardown, when `m_Registry.destroy()` skips `OnComponentRemoved`) ·
[vcpkg-dependency-management.md](vcpkg-dependency-management.md) (three of five traps silent —
including a forced port option that never applied) ·
[asset-import-usd-alembic.md](asset-import-usd-alembic.md) (winding, up-axis, unit scale, UV origin) ·
[mcp-protocol-eras.md](mcp-protocol-eras.md) (swapping the event stream's notification carrier drops
pushes that nothing reports missing — which is why the new carrier runs *beside* the deprecated one,
not instead of it) ·
[steamworks-platform-integration.md](steamworks-platform-integration.md) (an SDK path set one level
too high drops the whole feature — the build succeeds with Steam quietly switched off, and the error
explaining the correct layout is the one thing that never fires)

**The counter-move:** ask what the *absence* would look like, and whether anything in the system
would be different. If the answer is "nothing", you need a coverage test, not a unit test.

## 4. Your instrument is lying to you

The check you're using to verify the fix passes for a correct implementation *and* for a broken one.
Read before trusting any measurement.

| Doc | The instrument that failed |
|---|---|
| [live-verification-noise-floor.md](live-verification-noise-floor.md) | the flagship: a self-consistency crop check that a mirrored (wrong) position scored *better* on — and read-tools that answer 200 with a stale frame from an iconified window |
| [procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md) | a red that recurs every run gets normalised, blinding the whole suite |
| [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md) | a `glXxx(` grep is wrong three different ways; three published counts, none right |
| [volumetric-cloud-debugging.md](volumetric-cloud-debugging.md) | capture targets show the *editor* camera; include-only shader edits don't hot-reload |
| [timed-wait-test-assertions.md](timed-wait-test-assertions.md) | `duration_cast<milliseconds>` truncates toward zero — the "flaky test" may not be test-side at all |
| [shared-temp-dir-test-isolation.md](shared-temp-dir-test-isolation.md) | a CI comment asserted a safety property nobody had measured; and the issue's own repro had been fixed two months earlier |
| [reference-path-tracer.md](reference-path-tracer.md) | a golden answers "did it change?", never "is it correct?" |
| [vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md) | a per-vendor baseline set is the instrument validating itself — and a small cross-vendor RMSE measures portability, never correctness |
| [gpu-debug-draws.md](gpu-debug-draws.md) | read the two-counter overflow protocol before concluding "it drew nothing" |
| [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) §5e | the CI job that said `--config Release` was building no config at all, and grepping for `--parallel` finds four sites of which only one is a runaway — the generator, not the flag, decides |

**The counter-move:** measure the noise floor first, and construct a case the instrument *must*
fail on before trusting a case it passes.

## 5. Ordering and lifetime

The logic is right; when it runs, or how long it lives, is wrong.

[script-structural-command-safe-point.md](script-structural-command-safe-point.md) (mutating a
registry mid-iteration) · [visual-script-vm.md](visual-script-vm.md) (a loop node that forgets to
charge its own iteration hangs the frame; four things a graph must never do inline) · [runtime-scene-switching.md](runtime-scene-switching.md) (five ordering
rules for a swap that destroys the thing being iterated) ·
[follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) (input
before the physics kick, camera last) ·
[two-phase-occlusion-culling.md](two-phase-occlusion-culling.md) (pass order decides who still sees
previous-frame depth) ·
[virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) (clearing the LRU bit one step
early evicts the whole cache every frame; §8 — a perspective light face cannot be culled by
projecting AABB corners and dividing by w, and the caster vanishes only when it gets close) · [render-pass-published-state.md](render-pass-published-state.md) (publish
last, restore deliberately) · [cluster-lod-simplification.md](cluster-lod-simplification.md) (a lock
that must outlive the level that created it) ·
[render-graph-transient-aliasing.md](render-graph-transient-aliasing.md) (a read from a pooled
resource whose lifetime already ended) ·
[intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md) (TOCTOU between a
decrement and a re-read) · [per-frame-scratch-reuse.md](per-frame-scratch-reuse.md) (promoting a
per-tick local to persistent state) ·
[parallelizable-mover-systems.md](parallelizable-mover-systems.md) (splitting a system at its write
boundary) · [gl-clear-program-revalidation.md](gl-clear-program-revalidation.md) (what is *bound*
when you clear) ·
[vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) (a CPU write
between two recorded draws is GL command order; a life-stable Vulkan address makes it
last-write-wins) · [gpu-scan-compaction.md](gpu-scan-compaction.md) (a `barrier()` only some
invocations reach — the early-return habit in front of a work-group scan)

## 6. It was never actually called

Code that exists, is well-tested, and has no production caller — or has more callers than the
obvious grep suggests.

- [server-authoritative-networking-loop.md](server-authoritative-networking-loop.md) — ~20
  well-tested networking classes produced nothing, because the drive function had zero call sites
  and a comment claimed a wiring that didn't exist. **Grep for callers of the entry point, not for
  tests.**
- [crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) — `AddAgent` /
  `SetAgentTarget` had zero production callers for as long as they existed.
- [audio-voice-budget.md](audio-voice-budget.md) — the inverse: sounds start from **six** call
  sites, not the two a grep suggests, so the budget had to move inside `Play()`.
- [render-pass-published-state.md](render-pass-published-state.md) — `MeshComponent { Primitive: 0 }`
  is `None`: an entity that silently never renders.
- [virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) §5 — a render-graph pass's
  `Setup()` runs only when the frame graph is *rebuilt*, and the rebuild fingerprint does not
  include your feature's on/off setting. A `Setup()` that early-returns while the feature is off
  freezes that decision in: the pass keeps executing and keeps doing nothing. **The tell is that the
  feature works when it is enabled before the first frame and not when it is toggled on later.**

---

## Subsystem notes (`notes-*.md`)

A second, distinct family: **accumulated reference gotchas per subsystem**, rather than a postmortem
of one failure. Skim the relevant one before working in that area — each is a numbered list of
"this will bite you" facts.

- [notes-renderer.md](notes-renderer.md) — offline capture, the GL wrappers, shader bindings, SSAO/SSR/FSR1, IBL bakes, GPU timers.
- [notes-mcp-tool-authoring.md](notes-mcp-tool-authoring.md) — the three-part tool split, schemas, consent/undo, frame capture, what the live frame can honestly answer.
- [notes-core-and-threading.md](notes-core-and-threading.md) — yaml-cpp non-finite decode, `Ref<T>` constness, the task system, lock-free allocator testing, C++ traps.
- [notes-gameplay-physics-nav.md](notes-gameplay-physics-nav.md) — the two Jolt systems, joint/ragdoll wiring, Detour partial paths, dialogue guards, terrain/foliage.
- [notes-audio-animation-sim.md](notes-audio-animation-sim.md) — pose sampling by name, retargeting, morph targets, the fixed-timestep split, SoundGraph parameters and spatialization.
- [notes-editor-and-assets.md](notes-editor-and-assets.md) — Content Browser refresh, filewatch import, asset placeholders, texture cook, platform utils.

> These were salvaged in one pass from 229 memory files stranded across 143 deleted worktrees. See
> `docs/process/task-loop.md` Phase 7 for the rule that stops it recurring.

---

## Full roster

Everything above, plus the docs that are pure reference rather than postmortem.

**Standards** — [cpp-coding-quality.md](cpp-coding-quality.md) ·
[glsl-shaders.md](glsl-shaders.md) · [sonarqube-review-alignment.md](sonarqube-review-alignment.md)

**Testing** — [testing-architecture.md](testing-architecture.md) ·
[reference-path-tracer.md](reference-path-tracer.md) ·
[vendor-golden-baseline-crosscheck.md](vendor-golden-baseline-crosscheck.md) ·
[single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md) ·
[live-verification-noise-floor.md](live-verification-noise-floor.md) ·
[procedural-generator-golden-coupling.md](procedural-generator-golden-coupling.md) ·
[timed-wait-test-assertions.md](timed-wait-test-assertions.md) ·
[shared-temp-dir-test-isolation.md](shared-temp-dir-test-isolation.md)

**Build & deps** — [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) ·
[vcpkg-dependency-management.md](vcpkg-dependency-management.md) ·
[asset-import-usd-alembic.md](asset-import-usd-alembic.md)

**Renderer** — [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md) ·
[vulkan-command-ordered-buffer-writes.md](vulkan-command-ordered-buffer-writes.md) ·
[gpu-debug-draws.md](gpu-debug-draws.md) ·
[gpu-scan-compaction.md](gpu-scan-compaction.md) ·
[gl-clear-program-revalidation.md](gl-clear-program-revalidation.md) ·
[render-pass-published-state.md](render-pass-published-state.md) ·
[render-graph-transient-aliasing.md](render-graph-transient-aliasing.md) ·
[render-pipeline-caches.md](render-pipeline-caches.md) ·
[two-phase-occlusion-culling.md](two-phase-occlusion-culling.md) ·
[virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) ·
[cluster-lod-simplification.md](cluster-lod-simplification.md) ·
[terrain-gpu-lod-quadtree.md](terrain-gpu-lod-quadtree.md) ·
[camera-relative-rendering.md](camera-relative-rendering.md) ·
[distance-impostor-reflection-probes.md](distance-impostor-reflection-probes.md) ·
[foliage-impostor-card-rendering.md](foliage-impostor-card-rendering.md) ·
[light-path-photometric-parity.md](light-path-photometric-parity.md) ·
[volumetric-cloud-debugging.md](volumetric-cloud-debugging.md) ·
[pbf-solver-stability.md](pbf-solver-stability.md)

**Scene, ECS & serialization** — [component-serializer-codegen.md](component-serializer-codegen.md) ·
[scene-binary-sidecar.md](scene-binary-sidecar.md) ·
[binary-format-versioning.md](binary-format-versioning.md) ·
[scene-copy-must-carry-scene-level-settings.md](scene-copy-must-carry-scene-level-settings.md) ·
[floating-origin-rebase-subsystems.md](floating-origin-rebase-subsystems.md) ·
[asset-degradation-and-constructor-preconditions.md](asset-degradation-and-constructor-preconditions.md)

**Gameplay, physics & simulation** — [force-model-vehicles.md](force-model-vehicles.md) ·
[jolt-softbody-kinematic-attachment.md](jolt-softbody-kinematic-attachment.md) ·
[follow-camera-and-character-query-seams.md](follow-camera-and-character-query-seams.md) ·
[crowd-manager-follower-parity.md](crowd-manager-follower-parity.md) ·
[parallelizable-mover-systems.md](parallelizable-mover-systems.md) ·
[terrain-collision-streaming-sculpt.md](terrain-collision-streaming-sculpt.md) ·
[procedural-skinned-mesh-primitives.md](procedural-skinned-mesh-primitives.md) ·
[audio-voice-budget.md](audio-voice-budget.md) ·
[destructible-debris.md](destructible-debris.md)

**Scripting, networking & tooling** —
[script-structural-command-safe-point.md](script-structural-command-safe-point.md) ·
[visual-script-vm.md](visual-script-vm.md) ·
[runtime-scene-switching.md](runtime-scene-switching.md) ·
[server-authoritative-networking-loop.md](server-authoritative-networking-loop.md) ·
[mcp-setter-based-field-registry.md](mcp-setter-based-field-registry.md) ·
[mcp-protocol-eras.md](mcp-protocol-eras.md) ·
[steamworks-platform-integration.md](steamworks-platform-integration.md)

**Concurrency & memory** —
[intrusive-refcount-weakref-races.md](intrusive-refcount-weakref-races.md) ·
[spinlock-payload-cache-line-separation.md](spinlock-payload-cache-line-separation.md) ·
[per-frame-scratch-reuse.md](per-frame-scratch-reuse.md)

---

## Adding a doc here

The corpus grows roughly ten files a month, so consistency matters more than completeness.

1. **Write it when the bug is fixed, not later.** The value is in the wrong hypotheses you held on
   the way, and those evaporate within a day.
2. **Title it as the rule, not the topic** — "Drive a pinned vertex by velocity, not position", not
   "Cloth attachment notes". The title is what makes it findable in the Companion-guides list.
3. **Lead with what stayed green.** If the failure was caught by a test, it probably doesn't need a
   doc — the test is the artifact. These files exist for the failures tests *couldn't* see.
4. **Cite the issue number.** 45 of 46 existing docs do.
5. **Add two links**: one line in `CLAUDE.md` → *Companion guides* under its subsystem, and a row in
   whichever archetype above it belongs to. An unlinked doc is an unread doc — five files sat
   orphaned here before this index existed.
6. **Keep it under ~10 KB.** Past that it stops being read; split it or push the history into an
   appendix at the end.
