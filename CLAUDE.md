# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## Committing and publishing

Two contexts, different defaults.

**In a task worktree running the task loop** — a `feature/*` branch created by `/start-work`, session kicked off from `HANDOVER.md` — committing, pushing with `git push -u origin feature/<slug>`, opening the PR, and commenting on issues are **pre-authorized**. That is the job; see [docs/process/task-loop.md](docs/process/task-loop.md). Don't stop to ask at each step.

**Everywhere else** — the base repo, `master`, an ad-hoc session, any worktree you didn't get a handover for — ask first. "Commit this", "push the branch", "open a PR" are clear go-aheads; "we're done", "looks good", "wrap it up" are not. When work reaches a finishable state, offer once: "Want me to commit these N changes as `<draft message>`?"

**Gated in every context — these always need an explicit instruction, loop or not:**

- committing or pushing to `master` directly
- `git push --force` / `--force-with-lease`
- `git reset --hard`, rebasing already-pushed commits, `git commit --amend` on a pushed commit
- merging a PR (`gh pr merge`)
- closing a GitHub issue — post the evidence and recommend closure, let the user close

**Never a bare `git push`**, even inside the loop: under `push.default` it can fire straight at `master`, which has happened on a real branch here. Always `git push -u origin feature/<slug>`.

Always fine anywhere, no ask: edit files, `git checkout -b`, `git add`, `git status` / `git diff` / `git log`, and additive PR or issue comments.

---

## Definition of done — before you hand back to the user

When you finish a task that touched code or assets:

1. **Pre-commit is automatic.** A `Stop` hook in `.claude/settings.json` runs `pre-commit run --all-files` at the end of every turn (the wrapper script at `scripts/claude-stop-hook.ps1` is the source of truth — it runs the whole repo, not just modified files, on purpose). If it reformats anything, the changes are already on disk — re-stage them before committing. Do **not** run `pre-commit` again manually unless it failed. A git `pre-commit` hook also gates `git commit` directly (installed via `pre-commit install`; shared across all worktrees): a commit that triggers an auto-fix will abort with the fixes left unstaged — re-`git add` and commit again. This is expected, not a failure.
2. **Test-file classification** is enforced by the `test-catalogue-classified` hook above. If it fails, a test `.cpp` is unclassified — add a `// OLO_TEST_LAYER: <id>` comment near its top (preferred) or a `file_layer_map` entry in `test_catalogue.json`. The rendered catalogue tables are **generated, git-ignored** docs (`docs/test-catalogue.*.md`); regenerate them on demand with `python OloEngine/tests/scripts/generate_test_catalogue.py` — there is nothing to re-stage.
3. **Cross-binding check** — the pre-commit hook does **not** catch this. If you added or changed an ECS component, you must update the remaining hand-maintained touch-points or scripting / scene saves / save-games / runtime copies will silently drop it (and the build will fail to link):
   - `Scene/Components.h` — declare the component struct itself. The `AllComponents` tuple is **no longer hand-edited**: OloHeaderTool now generates it from every `struct *Component` *definition* under `OloEngine/src` into `Scene/Generated/AllComponents.Generated.inl`, which `Components.h` `#include`s at the bottom — declare the struct and the next build registers it for scene copy / prefab / `HasComponent<T>()` automatically. The exceptions are **runtime-only / hand-copied** components (entity-identity `IDComponent` / `TagComponent`, and per-tick derived `*StateComponent` / `UIResolvedRectComponent`): those must be excluded in **both** the generator's `kComponentsNotInTuple` set (`tools/OloHeaderTool/main.cpp`) **and** `ComponentTupleCoverageTest`'s `kNotInTuple` set — keep the two in sync. Still **guarded** by `OloEngine/tests/ComponentTupleCoverageTest.cpp` (both directions, now reading the generated file): it catches a stale generated tuple (rebuild `GenerateBindings`) and drift between the generator's recursive scan and the test's curated header-root list (a component in a header the test doesn't list surfaces in the inverse test → add the header to `ComponentHeaderRoots()`).
   - `Scene/Scene.cpp` — the `Scene::OnComponentAdded<T>` / `OnComponentRemoved<T>` **no-op specializations are no longer hand-edited** (issue #380, third slice): OloHeaderTool generates them from the same `struct *Component` scan into `Scene/Generated/OnComponent{Added,Removed}.Generated.inl`, which `Scene.cpp` `#include`s inside the `OLO_ON_COMPONENT_{ADDED,REMOVED}_NOOP` macros — so a new component that needs **no** init/teardown is registered automatically and you touch nothing here. You only edit this file when the component needs a **real body**: hand-write the `template<>` specialization (alongside the existing custom ones — `CameraComponent`, the physics group, audio/video, etc.) **and** add the component to the matching exclusion set in `tools/OloHeaderTool/main.cpp` (`kComponentsCustomOnAdd` and/or `kComponentsCustomOnRemove` — **two deliberately different sets**: e.g. `CameraComponent`/`CinematicComponent`/`LocalizedTextComponent` do real work on add but no-op on remove, while `Rigidbody2DComponent`/`SpringBoneComponent`/`NoiseAnimationComponent` are the reverse). Forgetting the exclusion-set edit ⇒ the generated no-op collides with your body (**duplicate-definition** compile error); forgetting the body while leaving it in the set ⇒ a **link error** (engine for add via `AddComponent<T>`, OloEditor for remove via the remove-component button). Both are loud, not silent. The non-`struct *Component` `Skeleton` is the one type the scan can't see, so its no-ops stay hand-written. Guarded by `OloEngine/tests/ComponentHandlerCoverageTest.cpp` (verifies each generated list == every component minus its custom set; mirrors both `kComponentsCustomOn{Add,Remove}` sets — keep them in sync, same contract as `ComponentTupleCoverageTest`).
   - `Scene/SceneSerializer.cpp` — `SerializeEntity` (writes) and `DeserializeEntityComponents` (reads). Validate every float with `std::isfinite`. **An all-trivial component needs zero edits here** (issue #380, fourth slice): OloHeaderTool generates both blocks into `Scene/Generated/Scene{Serialize,Deserialize}Components.Generated.inl`, which `SceneSerializer.cpp` `#include`s. Decide what to do with a new component like this: **a member type the classifier doesn't recognise** (`std::array`, a `std::vector` of a non-trivial struct, `Ref<T>` of a non-`Asset`-derived type — *or any non-public member*) ⇒ the component is skipped **automatically**, so hand-write the block and add **no** exclusion; **a runtime-only field** ⇒ tag it `OLO_SERIALIZE(Skip)`; **a range check** ⇒ tag the field `OLO_SERIALIZE(Clamp, Min=…, Max=…)` to saturate, or `OLO_SERIALIZE(Reject, Min=…, Max=…)` to fall back to the constructor default — use `Reject` for a *discriminated* value like an enum or a mode index, where saturating silently produces a **different valid** value; **anything else the plain round-trip can't express** (a cross-field invariant, a `Sanitize*`, a non-`m_`-stripped key, entity identity) ⇒ hand-write the block **and** add the component to `kComponentsCustomSerialize` (`tools/OloHeaderTool/main.cpp`), a **fourth** exclusion set distinct from the tuple / save-game / on-add / on-remove ones — forgetting it is a loud double-emit test failure, not a silent drop. **When you widen the classifier, rebuild `GenerateBindings` and diff the generated `.inl`**: every past slice flipped a component nobody predicted, and the one case the coverage tests cannot catch is a component persisted some way *other* than a sub-map (`IDComponent`). Full eligibility table, annotation reference, hand-written roster and slice history: [docs/agent-rules/component-serializer-codegen.md](docs/agent-rules/component-serializer-codegen.md).
   - **SaveGame — two edits in one file:** (a) `SaveGame/SaveGameComponentSerializer.h` — forward decl + `Serialize()` overload declaration; (b) `SaveGame/SaveGameComponentSerializer.cpp` — `Serialize()` overload definition + `RegisterAll()` registration. The `SAVE_COMPONENT(...)` capture list and `TRY_LOAD_COMPONENT(...)` restore list in `SaveGame/SaveGameSerializer.cpp` are **no longer hand-edited** (issue #380, second slice): OloHeaderTool generates them from the same `struct *Component` scan as the `AllComponents` tuple, minus a save-game-specific exclusion set, into `SaveGame/Generated/SaveGameComponent{Capture,Restore}.Generated.inl`, which `SaveGameSerializer.cpp` `#include`s — so giving a component a `Serialize()` overload makes the next build capture/restore it automatically. The exclusion set is `kComponentsNotInSaveGame` (`tools/OloHeaderTool/main.cpp`) and is **deliberately different** from `kComponentsNotInTuple`: save-games *keep* `IDComponent`/`TagComponent` but *drop* every component lacking a `Serialize()` overload (the four per-tick `*StateComponent` / `UIResolvedRectComponent`, **plus** `AudioSoundGraphComponent` / `LocalizedTextComponent`, which are in the scene-copy tuple but have no save serializer yet). A component with a serializer but wrongly listed in that set — or vice versa — is caught by `SaveGameComponentSerializerCoverageTest` (which now parses the generated `.inl` files; rebuild `GenerateBindings` after editing `RegisterAll`). A component that needs saving but lacks a `Serialize()` overload is **still** silently dropped through save-games while round-tripping through scene YAML — so the `Serialize()` overload + `RegisterAll()` registration remain the two edits you must not forget.
   - `Scripting/C#/Generated/` is auto-generated from `OLO_PROPERTY` annotations by OloHeaderTool (see below) — add the annotations on the component fields. Don't hand-edit the generated `.inl` / `.cs`.
   - `Scripting/Lua/LuaScriptGlue.cpp::RegisterAllTypes()` — Sol2 usertype registration. (`OloEngine-LuaScriptCore/` is the Mono-equivalent project target but the actual bindings live in `Scripting/Lua/`.) **Intentionally NOT completeness-guarded** — many components legitimately aren't Lua-exposed, so only per-component *functional* round-trips exist (`tests/Lua/LuaBindingTest.cpp`); a completeness test here would be noise. This is the one cross-binding touch-point with no completeness guard, by design.
   - `OloEditor/src/Panels/SceneHierarchyPanel.cpp` — **two** hand-maintained per-component lists: a `DrawComponent<T>(...)` call for the inspector row (95 of them) and a `DisplayAddComponentEntry<T>("Label")` for the Add Component menu (94 — the counts differ on purpose; `TransformComponent` is inspectable but not user-addable, since every entity already has one). Skipping this is not a build or test failure: the component round-trips through scene YAML and save-games perfectly and is simply **invisible and uneditable in the editor**, which is why it is easy to miss for several PRs. Also un-guarded by any completeness test — the same trade-off as the Lua bullet above, but for a different reason: the editor list is a UX judgement, not a mechanical mapping. See the undo/redo section below for which of the three `DrawComponent<T>` tiers your component lands in.

   Missing any one causes silent script or scene failures (or, for `OnComponentAdded`, a link error). Audit the generated `AllComponents` list (`Scene/Generated/AllComponents.Generated.inl`, produced by OloHeaderTool and `#include`d at the bottom of `Scene/Components.h`) as the source of truth for what's expected to round-trip. A separate `ComponentSerializerCoverageTest` greps a fixed header list — if the component lives in a new header, add it there too (`ComponentTupleCoverageTest` keeps an identical header-root list; update both).

If you find yourself wanting to write "remember to run pre-commit" anywhere, don't — the hook owns that.

---

## Companion guides

Read the relevant one before doing anything non-trivial; do not duplicate their content here. Each
is a postmortem of a real failure — most of them failures the test suite stayed green through.
[docs/agent-rules/README.md](docs/agent-rules/README.md) indexes the same set by **failure mode**
(green-but-wrong, two-mirrors-drift, silent drop, …) for when you don't yet know which subsystem
you are in.

**Code & review standards**

- [cpp-coding-quality.md](docs/agent-rules/cpp-coding-quality.md) — init-statements, float comparison, `auto`, IWYU, the defaulted `operator==` MSVC quirk.
- [glsl-shaders.md](docs/agent-rules/glsl-shaders.md) — the SPIR-V rules that make a shader fail to compile: bare uniforms, UBO bindings, MRT outputs.
- [sonarqube-review-alignment.md](docs/agent-rules/sonarqube-review-alignment.md) — read before `/code-review` so local findings match the cloud C++ Extended profile.

**Testing & verification**

- [testing-architecture.md](docs/agent-rules/testing-architecture.md) — which of the renderer's 11 layers or the Functional axis a new test belongs to, and the registration contract.
- [docs/testing.md](docs/testing.md) — *why* we test what we test: value heuristic, named anti-patterns, retirement criteria.
- [reference-path-tracer.md](docs/agent-rules/reference-path-tracer.md) — the ground-truth oracle: how to ask "is it *correct*?" when a golden can only answer "did it change?".
- [single-mesh-visual-test-lighting.md](docs/agent-rules/single-mesh-visual-test-lighting.md) — a sparse visual-test scene renders the subject near-black; add a ground plane, then look at the PNG.
- [live-verification-noise-floor.md](docs/agent-rules/live-verification-noise-floor.md) — measure the frame-to-frame noise floor before attributing any pixel difference to your change, and confirm the editor is drawing frames at all.
- [procedural-generator-golden-coupling.md](docs/agent-rules/procedural-generator-golden-coupling.md) — a determinism/quality fix to a generator invalidates every golden that captured it; fix and rebake must ship in the same PR.
- [timed-wait-test-assertions.md](docs/agent-rules/timed-wait-test-assertions.md) — a millisecond-granularity lower bound on a timed wait is a flake trap; measure in µs and assert one-sided.
- [shared-temp-dir-test-isolation.md](docs/agent-rules/shared-temp-dir-test-isolation.md) — every gtest case is its own process, so a fixed temp path is cross-process shared state; use `TestTempDir.h`, never `temp_directory_path()`.

**Build & dependencies**

- [build-trees-and-windows-asan.md](docs/agent-rules/build-trees-and-windows-asan.md) — never build the msvc and clangcl trees concurrently; plus per-user `mspdbsrv` stalls, the codegen build-graph wiring, and the local ASan recipe.
- [vcpkg-dependency-management.md](docs/agent-rules/vcpkg-dependency-management.md) — read before adding, bumping or removing a dep: the CRT triplet mismatch is heap corruption, and three of the five traps are silent.
- [asset-import-usd-alembic.md](docs/agent-rules/asset-import-usd-alembic.md) — the importer/exporter registry seam, and vendoring OpenUSD / Alembic / MaterialX into a static-everything build.

**Renderer**

- [rhi-abstraction-boundary.md](docs/agent-rules/rhi-abstraction-boundary.md) — where the OpenGL boundary actually leaks: the include graph, not a `glXxx(` grep; plus the Vulkan epic's per-phase lessons (§9–§13, incl. the one-row-order-per-backend contract).
- [vulkan-command-ordered-buffer-writes.md](docs/agent-rules/vulkan-command-ordered-buffer-writes.md) — a CPU buffer write between two recorded draws is GL command order; a life-stable Vulkan address silently makes it last-write-wins, and the failure is scene-shaped.
- [lazy-static-release-ownership.md](docs/agent-rules/lazy-static-release-ownership.md) — a shared lazy static released from `Renderer3D::Shutdown` leaks in every session that never inits 3D; GL is silent about it, Vulkan is not.
- [gpu-debug-draws.md](docs/agent-rules/gpu-debug-draws.md) — **the instrument for GPU-driven work**: any shader can draw a primitive into the viewport the same frame. Read the two-counter overflow protocol before concluding "it drew nothing".
- [gpu-scan-compaction.md](docs/agent-rules/gpu-scan-compaction.md) — the `if (idx >= count) return;` habit is a hang in front of a work-group scan, and a compaction test that compares *sets* passes on the bug it is meant to catch.
- [gl-clear-program-revalidation.md](docs/agent-rules/gl-clear-program-revalidation.md) — NVIDIA recompiles the *bound* program at `glClear`; wrap any new clear site in `GLClearProgramGuard` (unbind **and restore**).
- [render-pass-published-state.md](docs/agent-rules/render-pass-published-state.md) — a pass whose outputs are engine-global bindings must publish last, and must not be wrapped in `GLStateGuard(Restore)`.
- [render-graph-transient-aliasing.md](docs/agent-rules/render-graph-transient-aliasing.md) — `WriteNewVersion` renames a physical resource; the stale-pool-read archetype, and the poison/disable levers that find it.
- [render-pipeline-caches.md](docs/agent-rules/render-pipeline-caches.md) — process-wide render caches must invalidate on every topology reset, not just on a fingerprint change.
- [two-phase-occlusion-culling.md](docs/agent-rules/two-phase-occlusion-culling.md) — phase 1 must test the previous frame's FINAL pyramid; pass order decides who still sees previous-frame depth.
- [virtual-shadow-map-page-cache.md](docs/agent-rules/virtual-shadow-map-page-cache.md) — four page-cache invariants; break one and the frame still renders, wrong, as a different bug. §5 is general: a render-graph `Setup()` that branches on a runtime toggle is frozen by the frame-graph fingerprint cache.
- [cluster-lod-simplification.md](docs/agent-rules/cluster-lod-simplification.md) — a terminal group's boundary lock must outlive the level that created it; plus how to A/B a builder change the editor never runs.
- [terrain-gpu-lod-quadtree.md](docs/agent-rules/terrain-gpu-lod-quadtree.md) — the GPU terrain descent: crack-freedom is a vertex-set property, and a gating flag no scene sets is a feature with zero coverage.
- [camera-relative-rendering.md](docs/agent-rules/camera-relative-rendering.md) — every world-space GPU upload is a site; large-coordinate f32 cancellation shows up as vertex jitter and shadow swim.
- [distance-impostor-reflection-probes.md](docs/agent-rules/distance-impostor-reflection-probes.md) — one encoding contract mirrored in three places, and the miss-sentinel that shades from stale sky.
- [foliage-impostor-card-rendering.md](docs/agent-rules/foliage-impostor-card-rendering.md) — three ways impostor cards go missing, separable only by reading the PNG from several azimuths.
- [light-path-photometric-parity.md](docs/agent-rules/light-path-photometric-parity.md) — the three light evaluators must agree; a dropped field in a GPU light struct is a silently dead knob.
- [volumetric-cloud-debugging.md](docs/agent-rules/volumetric-cloud-debugging.md) — four look-alike causes of a "uniform veil", plus the two suite-level golden killers.
- [pbf-solver-stability.md](docs/agent-rules/pbf-solver-stability.md) — PBF/SPH formulas assume unit-mass particles; with physical masses the reference constants diverge.

**Scene, ECS & serialization**

- [component-serializer-codegen.md](docs/agent-rules/component-serializer-codegen.md) — when a component round-trips for free, when to annotate a field, and when to hand-write *and* exclude.
- [scene-binary-sidecar.md](docs/agent-rules/scene-binary-sidecar.md) — the `.scenebin` fast path: generated, hybrid-covered, and how it gets invalidated.
- [binary-format-versioning.md](docs/agent-rules/binary-format-versioning.md) — versioning a fixed-order binary archive: gate each new field, don't trust the header check to exclude old data.
- [scene-copy-must-carry-scene-level-settings.md](docs/agent-rules/scene-copy-must-carry-scene-level-settings.md) — `Scene::Copy()` drops scene-level settings structs the instant Play starts, invisibly to headless tests.
- [floating-origin-rebase-subsystems.md](docs/agent-rules/floating-origin-rebase-subsystems.md) — the four subsystems holding world-space state outside the rebased set, each needing a different fix.
- [asset-degradation-and-constructor-preconditions.md](docs/agent-rules/asset-degradation-and-constructor-preconditions.md) — a precondition asserted in a constructor delegates safety to ~30 call sites, and they will disagree.

**Gameplay, physics & simulation**

- [force-model-vehicles.md](docs/agent-rules/force-model-vehicles.md) — boats and aircraft driven by `AddForce`: every bug in this document leaves the test suite green.
- [jolt-softbody-kinematic-attachment.md](docs/agent-rules/jolt-softbody-kinematic-attachment.md) — drive a pinned cloth vertex by velocity, never by position.
- [follow-camera-and-character-query-seams.md](docs/agent-rules/follow-camera-and-character-query-seams.md) — a `CharacterVirtual`'s inner body is invisible to every UUID-keyed body filter, and a follow camera must run last.
- [crowd-manager-follower-parity.md](docs/agent-rules/crowd-manager-follower-parity.md) — a valid navmesh silently switches every `NavAgentComponent` onto the crowd follower.
- [parallelizable-mover-systems.md](docs/agent-rules/parallelizable-mover-systems.md) — how to move an entity-moving system onto a worker thread, and the two traps in the determinism test itself.
- [terrain-collision-streaming-sculpt.md](docs/agent-rules/terrain-collision-streaming-sculpt.md) — streamed tile bodies live in a second `JoltScene` map, and tile pose must reproduce the draw transform exactly.
- [procedural-skinned-mesh-primitives.md](docs/agent-rules/procedural-skinned-mesh-primitives.md) — code-built test meshes need a live GL context and a shared bind-pose origin.
- [destructible-debris.md](docs/agent-rules/destructible-debris.md) — pre-authored debris chunks, not runtime mesh fracture; two unrelated physics layer numberings mean `SetCollisionLayer(Debris)` never reaches Jolt's `ObjectLayers::DEBRIS`.
- [audio-voice-budget.md](docs/agent-rules/audio-voice-budget.md) — the voice cap must be admitted inside `Play()`, because the engine starts sounds from six call sites.

**Scripting, networking & tooling**

- [script-structural-command-safe-point.md](docs/agent-rules/script-structural-command-safe-point.md) — a script binding that changes the registry structurally must queue a command, never act inline.
- [visual-script-vm.md](docs/agent-rules/visual-script-vm.md) — the node-graph VM: a loop node must charge its own iteration, memoization is per exec step, and `PinType`'s numbering is on disk.
- [runtime-scene-switching.md](docs/agent-rules/runtime-scene-switching.md) — the host applies a scene swap after the tick; five ordering rules, plus the `Project` mount shipped games were missing.
- [server-authoritative-networking-loop.md](docs/agent-rules/server-authoritative-networking-loop.md) — ~20 well-tested networking classes produced nothing, because the entry point had zero call sites.
- [mcp-setter-based-field-registry.md](docs/agent-rules/mcp-setter-based-field-registry.md) — the copy-then-swap MCP write path is unsound when `operator=` can't reproduce a field write's side effects.
- [mcp-protocol-eras.md](docs/agent-rules/mcp-protocol-eras.md) — the 2026-07-28 stateless core is a second transport, not a field addition; `server/discover` added on its own actively breaks working clients.
- [steamworks-platform-integration.md](docs/agent-rules/steamworks-platform-integration.md) — a public repo can't hold the SDK, so CI can't build the enabled path; plus the import-lib DLL that every exe needs and the auto-detect that fails silently.

**Concurrency & memory**

- [intrusive-refcount-weakref-races.md](docs/agent-rules/intrusive-refcount-weakref-races.md) — a decrement-then-reread of a refcount is a TOCTOU double-free even when both operations are atomic.
- [spinlock-payload-cache-line-separation.md](docs/agent-rules/spinlock-payload-cache-line-separation.md) — a lock sharing a cache line with its payload costs the owner an invalidate round-trip on every acquire.
- [per-frame-scratch-reuse.md](docs/agent-rules/per-frame-scratch-reuse.md) — three things to check before promoting a per-tick scratch vector to persistent state.

**Subsystem notes** — accumulated per-area gotchas (a different genre from the postmortems above; skim the relevant one before working in that area)

- [notes-renderer.md](docs/agent-rules/notes-renderer.md) — offline capture, GL wrappers, shader bindings, SSAO/SSR/FSR1, IBL bakes, GPU timers.
- [notes-mcp-tool-authoring.md](docs/agent-rules/notes-mcp-tool-authoring.md) — the three-part tool split, schemas, consent/undo, frame capture, what the live frame can honestly answer.
- [notes-core-and-threading.md](docs/agent-rules/notes-core-and-threading.md) — yaml-cpp non-finite decode, `Ref<T>` constness, the task system, lock-free allocator testing, C++ traps.
- [notes-gameplay-physics-nav.md](docs/agent-rules/notes-gameplay-physics-nav.md) — the two Jolt systems, joint/ragdoll wiring, Detour partial paths, dialogue guards, terrain/foliage.
- [notes-audio-animation-sim.md](docs/agent-rules/notes-audio-animation-sim.md) — pose sampling by name, retargeting, morph targets, the fixed-timestep split, SoundGraph parameters and spatialization.
- [notes-editor-and-assets.md](docs/agent-rules/notes-editor-and-assets.md) — Content Browser refresh, filewatch import, asset placeholders, texture cook, platform utils.

**Operations**

- [docs/ops/build.md](docs/ops/build.md) — the full Windows / Linux / WSL build matrix.

---

## Build & run

**`VCPKG_ROOT` must be set** — since issue #773 third-party dependencies come from the [vcpkg.json](vcpkg.json) manifest, and every preset points `CMAKE_TOOLCHAIN_FILE` at `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`. One-time per machine:

```powershell
git clone https://github.com/microsoft/vcpkg D:\vcpkg
D:\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT D:\vcpkg          # persists for FUTURE shells only
$env:VCPKG_ROOT = "D:\vcpkg"      # ...so set it in THIS one too, or open a new shell
git -C D:\vcpkg config core.fsmonitor false   # or every `vcpkg install` fails on a file lock
```

A configure without it stops at a guard in the root `CMakeLists.txt` pointing at [docs/agent-rules/vcpkg-dependency-management.md](docs/agent-rules/vcpkg-dependency-management.md) — read that before touching `vcpkg.json`, `cmake/triplets/`, or `cmake/overlay-ports/`.

**`STEAMWORKS_SDK_ROOT` is optional and developer-supplied** (issue #644). The Steamworks SDK is *not* in this repo and cannot be: its licence forbids redistribution and this repo is public, so there is no vendored copy and no vcpkg port — each developer downloads it themselves (free; the Steamworks SDK Access Agreement, not the $100 Steam Direct publishing fee) and points the variable at the inner `sdk` directory, the one *directly* containing `public/` and `redistributable_bin/`. `OLO_WITH_STEAM` then auto-detects to ON; without the variable it is OFF and the whole engine builds, runs and tests normally. Setup and traps: [docs/ops/build.md](docs/ops/build.md#steamworks-sdk-optional--you-must-obtain-it-yourself). Two consequences worth not re-deriving: **CI can never build the enabled path**, so a hand-written stub SDK (`-DOLO_WITH_STEAM_STUB_SDK=ON`, `.github/workflows/steam-stub.yml`) compiles/links/runs it instead — add any new Steamworks call to the stubs or that job goes red; and **exactly one TU may include a Valve header** (`Platform/Steam/SteamworksBackend.cpp`), enforced by a CI grep, because that is what keeps the logic fake-testable. Everything else talks to `ISteamBackend`.

CMake presets ([CMakePresets.json](CMakePresets.json)) — note all three require **CMake 4.2+** because the `msvc` preset's `Visual Studio 18 2026` generator only exists in CMake 4.2 and the others inherit / share the version requirement at the top of the file:

- `msvc` (Visual Studio 18 2026, `build/`) — primary; default build dir. Triplet `x64-windows-static-md`.
- `clangcl` (Ninja Multi-Config, `build-clang/`) — clang-cl warnings with MSVC ABI. Compiles **our** code with clang-cl (chainloaded via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`) but shares the **`x64-windows-static-md`** ports with the msvc tree — deliberately, see [docs/agent-rules/vcpkg-dependency-management.md](docs/agent-rules/vcpkg-dependency-management.md).
- `clangcl-asan` — adds AddressSanitizer.

```powershell
# Generate VS solution
scripts\Win-GenerateProjectVS2022.bat   # or VS2026

# Build a target
cmake --build build --target OloEditor       --config Debug --parallel 6
cmake --build build --target OloEngine-Tests --config Debug --parallel 6
cmake --build build --target OloRuntime      --config Debug --parallel 6
cmake --build build --target OloServer       --config Debug --parallel 6

# ClangCL (configure once, then build)
cmake --preset clangcl
cmake --build build-clang --target OloEngine-Tests --config Debug --parallel 6
```

### Every build goes through the mutex

Not the bare commands above — wrap them:

```powershell
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build --target OloEngine-Tests --config Debug --parallel 6'
```

[build-lock.ps1](.claude/skills/run-oloengine/build-lock.ps1) runs one build at a time across every worktree of this repo, and kills its own build if the session that launched it dies. A `PreToolUse` hook (`scripts/claude-build-lock-guard.py`) **blocks** a `cmake --build` / `ninja` / `msbuild` tool call that skips it and replies with the wrapped command to use; a command that merely *mentions* a build tool opts out with the literal marker `OLO_BUILD_LOCK_BYPASS`. Why it exists, and what it does and does not protect you from: [.claude/skills/run-oloengine/SKILL.md](.claude/skills/run-oloengine/SKILL.md).

### Cap build parallelism — a full-width build can OOM this machine

**Never build uncapped.** Either pass an explicit job count — `--parallel 6`, or `ninja -j6` — or set `CMAKE_BUILD_PARALLEL_LEVEL`, which `cmake --build` uses whenever no `--parallel` is given (this is how the nightly workflow caps itself):

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = "6"   # PowerShell (the primary dev shell here)
```

```bash
export CMAKE_BUILD_PARALLEL_LEVEL=6      # POSIX shell / the Linux GPU runner
```

An explicit `--parallel N` overrides the environment variable, so don't set one expecting the other to win.

**`CMAKE_BUILD_PARALLEL_LEVEL` caps `cmake --build` only — `ninja` does not read it.** A direct `ninja` invocation must always carry a numeric `-jN` of its own; setting the variable and then running bare `ninja` gives you the full 18-wide default with no warning. What is never acceptable is a bare `--parallel`, or a direct `ninja` without `-jN`.

This is not a style preference. The dev box (an i7-14700KF, 20 cores / 28 threads / 64 GB — issue #759 corrected the earlier 16c/31GB figure) *also* hosts the `gh-runner-1/2/3` runners for another repository, so a build never has the machine to itself. Neither default is a cap:

- `cmake --build … --parallel` with **no number** does not pick a number itself — it forwards the omission to the native build tool, whose own default applies (unless `CMAKE_BUILD_PARALLEL_LEVEL` is set). So the width you get depends on the generator, and it is never *lower* than the tool's default.
- With Ninja that default is `cores + 2` — 30 on this host, confirmed by `ninja --help` reporting `[default=30 on this system]`. Dropping a `--parallel N` flag therefore *raises* the width rather than lowering it.

An agent session running repeated uncapped builds — especially with a test suite running alongside — has already OOM-killed this host once. If you need it faster, use ccache (already wired in), not more jobs.

**Measured headroom (issue #759).** `-j6` is a deliberately conservative *floor*, not a hard limit for this hardware. An instrumented clean Debug build on this idle 64 GB host peaks at **~47 GiB (MSVC) / ~42 GiB (clang-cl)** at `-j6` — and because a handful of heavy TUs set the peak, not the lane count, `-j12` adds only **~2 GiB** of peak while running **1.3–1.6× faster** (still ~16 GiB free). So `-j12` is a safe, faster choice **when this box is otherwise idle**. Keep `-j6` as the default: it is the value that stays safe on the smaller/shared worst case and while the CI runners are active — the constraint the pool and cap were written for is the *shared* box, not the raw memory ceiling. The link pool (`OLO_LINK_JOBS=2`) is separately confirmed correct: linking is off the compile-bound critical path, so the pool costs ~0 wall-time. Details and the trace method are in [docs/agent-rules/build-trees-and-windows-asan.md](docs/agent-rules/build-trees-and-windows-asan.md) §5.

Link steps are capped separately and automatically: the root `CMakeLists.txt` puts them in a Ninja job pool (`OLO_LINK_JOBS`, default 2) because linking the full static engine is the memory spike. That pool lives in the generated `build.ninja`, so it protects a bare `ninja` too — but it does **not** cap compilation, which is what the job count above is for.

VS Code tasks ([.vscode/tasks.json](.vscode/tasks.json)) wrap the above: `build-oloeditor-debug`, `run-oloeditor-debug`, `build-tests-debug`, `run-tests-debug`, `build-clangcl-tests-debug`, `configure-clangcl`, etc.

**Working directory matters.** `OloEditor`, `OloRuntime`, and `OloServer` resolve assets, shaders, and Mono assemblies relative to `OloEditor/`. Always run with `cwd = OloEditor/` (the VS Code tasks already do this; the test binary runs from repo root instead).

Targets: `OloEngine` (static lib), `OloEditor`, `OloRuntime`, `OloServer`, `OloEngine-Tests`, plus `OloEngine-LuaScriptCore` and (Visual Studio generator only) `OloEngine-ScriptCore` for C#. `OloEditor` `add_dependencies` on `OloRuntime` so the BuildGamePanel never ships a stale runtime. `OloEngine` and `OloEngine-ScriptCore` both depend on the `GenerateBindings` target — see "OloHeaderTool" below.

---

## Tests

GoogleTest, registered via [OloEngine/tests/CMakeLists.txt](OloEngine/tests/CMakeLists.txt). Run with the `run-tests-debug` task or:

```powershell
build\OloEngine\tests\Debug\OloEngine-Tests.exe
build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=SuiteName.TestName
```

Tests live on **two independent axes** that share only GoogleTest and the registration hook:

1. **Renderer testing pyramid** (L1–L11 + plumbing/cullinglod/shaderpipe/integration/meta) — pixels, GL state, shader math.
2. **Functional / cross-subsystem axis** (tag `"Functional"`) — Animation × Physics × Scripting × Networking × Audio × Asset × Nav × Save-game × Gameplay × AI seams driven by real `Scene::OnUpdateRuntime`.

The decision tree, per-layer reference, and anti-patterns all live in [docs/agent-rules/testing-architecture.md](docs/agent-rules/testing-architecture.md) and [docs/testing.md](docs/testing.md). When adding an engine feature, pick the right classification(s) from there before writing code.

Special rebase modes: `--olo-golden-rebase` for goldens, `--olo-perf-rebase` for perf baselines — only after a deliberate visual change or a hardware/optimisation move.

### Rendering changes MUST be visually verified — unit tests are not enough

A renderer change can pass every CPU/contract test and still look completely broken on screen (transparent water, foam wash, fog flooding the frame, z-fighting, a hard seam at the waterline). **Math/contract tests prove the formula; they do not prove the frame looks right.** So for any change that affects what the screen shows — shaders, render passes, materials, post-processing, culling, blending, a new visual component — do **all** of:

1. **Pin the math with CPU/contract tests** (the cheapest layer that proves the formula) — e.g. the fog/opacity/face-selection logic in [WaterRenderingTest.cpp](OloEngine/tests/Rendering/WaterRenderingTest.cpp). These run in CI.
2. **Capture screenshot evidence from multiple camera angles and actually look at the pixels.** Follow the visual-probe pattern in [SphereAreaLightVisualTest.cpp](OloEngine/tests/Rendering/PropertyTests/SphereAreaLightVisualTest.cpp) and [WaterVisualEvidenceTest.cpp](OloEngine/tests/Rendering/PropertyTests/WaterVisualEvidenceTest.cpp): render the real pipeline to a framebuffer, read it back, `stbi_write_png` to `OloEditor/assets/tests/visual/`, and **read the PNGs back** to confirm the result. Cover the cases where the effect is most likely to break — for water that meant *from the side*, *straddling the waterline*, *fully submerged*, and *looking down from above*. Don't trust "the test passed"; trust the image.
3. **Run the change in the editor** (`run-oloeditor-debug`, scene under `OloEditor/SandboxProject/Assets/Scenes/`) and check the `OloEngine.log` for shader compile/link errors. Add a representative test scene if one doesn't exist.

**Inspect the *live* editor frame over MCP.** When an editor is running you can drive it from this session without touching the user's viewport: the `run-oloengine` skill's `attach` action (`driver.ps1 -Action attach`) launches the editor with its read-only MCP diagnostics server auto-started on a per-worktree port and registers it with Claude Code. Then `olo_screenshot` (optional one-shot `camera`/`orbit` pose), `olo_camera_get` / `olo_camera_set_pose` / `olo_camera_orbit` / `olo_camera_frame_entity`, `olo_shader_errors`, and `olo_render_capture_target` (intermediate buffers — depth, normals, G-buffer, shadow map, AO, post stages) let you verify the rendered frame from multiple angles and see which buffer broke. See [docs/guides/mcp-diagnostics-server.md](docs/guides/mcp-diagnostics-server.md).

**Notice a missing MCP capability while working? Log it before moving on.** If engine work makes you reach for an `olo_*` tool that doesn't exist yet (e.g. wanting to export the render-graph topology or a command bucket for analysis), add a one-line bullet — tool name + what it should expose + why — to the open **"MCP: post-#357 follow-ups"** tracker (**#607**; the historical trackers #306 / #316 / #357 are all closed) rather than silently working around it. If no MCP tracker is open, file a fresh issue. For a **project-specific** diagnostic an agent could compose from existing read-only tools, consider a Lua script tool instead — drop a `RegisterMcpTool{...}` script into `<project assets>/McpTools/` (see the guide's "Script-defined tools" section) — and still log the gap if a native tool would serve every project. The MCP server is meant to grow from real usage friction; don't let the gap evaporate.

Do not report a rendering change as done on the strength of unit tests alone — that produced multiple "tests green, screen wrong" rounds. If you cannot capture/inspect a frame for a given change, say so explicitly.

**Visual-regression tests now run in the suite (issue #258, mostly closed):** the `RendererAttachedTest` fixture's `SetRenderingEnabled(false)` default is now an *opt-out*, not a hard limit. Call `EnableRendering(w, h)` and drive the render through a guarded helper — `RunFrames` (runtime primary camera) or `RunEditorFrames` (an explicitly posed `EditorCamera`, for multi-angle screenshots). Each wraps the full-pipeline tick in a `GLStateGuard(Restore)`, so a render no longer poisons later GPU tests; a process-wide `Renderer::Shutdown()` after `RUN_ALL_TESTS()` fixes the old teardown SIGSEGV. With that, `SceneRenderEvidenceTest` (lit cube) and `WaterVisualEvidenceTest` (water from 6 angles, golden-PNG + driver-independent contracts) run in the normal suite and **SKIP cleanly** (not fail) when no GL 4.6 context exists — so they gate any GPU-equipped run while headless CI skips them. New screenshot tests should follow this pattern instead of `DISABLED_`. `AssetSceneLoadTest` (loads **every** sandbox scene through `SceneSerializer::Deserialize` with the editor asset manager mounted — the full "File → Open Scene" path) now runs too: it brings the renderer up and **SKIPs cleanly** when no GL 4.6 context, same gate as above. Its old `DISABLED_` diagnosis ("a separate deserializer-refactor root cause, not GL-state hygiene") was **wrong** — the deserialiser eagerly builds GPU resources (`MeshSource::Build`, `Texture2D::Create`, `Font::Create`, shader-graph compiles), so it just needed a live context like the visual tests. The one genuine deserialiser bug it surfaced: `ScriptEngine::GetEntityClass` dereferenced its null `s_Data` when the C# engine wasn't initialised (a `ScriptComponent` then SEH-crashed deserialise) — now null-guarded, so a scene with a script loads with scripting off.

---

## Architecture

C++23 baseline, OpenGL 4.6 with DSA. Layout:

- `OloEngine/src/OloEngine/<Subsystem>/` — engine library. Subsystems: `Renderer`, `Scene`, `Physics3D`, `Asset`, `Audio`, `Animation`, `Scripting`, `Networking`, `UI`, `Particle`, `Navigation`, `Precipitation`, `Snow`, `Wind`, `Terrain`, `Dialogue`, `SaveGame`, `Server`, `AI`, `Gameplay`, plus core `Core`, `Memory`, `Threading`, `Task`, `Async`, `Containers`, `Templates`, `Math`, `Events`, `Serialization`, `Project`, `Build`, `Debug`, `Misc`, `Utils`, `Experimental`, `Algo`, `HAL`, `ImGui`.
- `OloEngine/src/Platform/` — platform-specific implementations (sibling of `OloEngine/`, not under it). Per-OS / per-API glue lives here.
- `OloEditor/` — ImGui editor. Panels under `src/`; runtime assets under `assets/`; sample game under `SandboxProject/`.
- `OloRuntime/` — standalone game runtime that loads what the editor builds.
- `OloServer/` — headless dedicated server (the only target that runs on WSL2).
- `OloEngine-ScriptCore/` (C# / Mono, Windows only) and `OloEngine-LuaScriptCore/` (Lua / Sol2, all platforms).
- `OloEngine/vendor/` — `FetchContent` / CPM downloads. **Never edit; CMake reconfigure wipes changes.**

Cross-cutting patterns:

- **ECS:** EnTT under an `Entity` wrapper (UUID). Components are POD-ish; serialization lives in [OloEngine/src/OloEngine/Core/YAMLConverters.h](OloEngine/src/OloEngine/Core/YAMLConverters.h). The hottest multi-component update loops use EnTT **owning groups** (`registry.group<Owned>(entt::get<Observed>)`) instead of views for packed iteration. EnTT v4 asserts only one rule: a component may be *owned* by at most one group (`get<>`-observed types are unconstrained), so `TransformComponent` — owned by the 2D sprite loop — is *borrowed* by the physics-sync / particle / audio groups. Before adding a new owning group, consult and update the ownership-map comment at the top of `Scene/Scene.cpp`; owning an already-owned component is a **runtime assert**, and an owned pool can't be `.sort()`ed.
- **Gameplay system scheduler (issue #453):** `Scene::SimulateRuntimeStep` no longer hard-codes the update order — each per-tick gameplay system registers in `Scene::GetGameplayScheduler()` (`Scene/Scene.cpp`) with declared `Reads/Writes/After/Before` constraints against the **named channel constants** (`GameplayChannel::k*` in `Scene.cpp` — never raw string literals, a typo'd channel silently drops its edges), and the execution order is **derived** (`Scene/SystemScheduler.{h,cpp}`: Kahn topological sort, registration order as the deterministic tie-break; duplicate name / dangling reference / cycle throws `SystemSchedulerError` in every build config). When adding or reordering a system, declare the **real data flow**, not just a position: a missing edge is invisible in the sequential order (the tie-break masks it) but becomes a data race under the parallel executor — so the seam test (`SystemSchedulerTest.GameplayScheduleHonoursDocumentedSeams`) asserts **reachability** (`SystemScheduler::DependsOn`), never positions. Two parallelism mechanisms, with different audit bars: **(1) the physics shadow** — the ECS-free physics world step (Box2D + Jolt world update) runs as an engine task between the `PhysicsKick` and `PhysicsFence` nodes (`Scene::KickPhysicsStep`/`FencePhysicsStep`; the ECS-touching phases — contact-event drain, character/vehicle input, joint-break, transform sync — were hoisted into kick/fence on the game thread, see the `JoltScene` phase methods). Systems registered between kick and fence run **on the game thread** during the step and need NO worker-thread-safety audit — only independence from physics/transform state (current occupants: Dialogue, Quest — structural changes and bus publishes are fine there). **(2) `.Parallelizable()` worker dispatch** — ONLY after a thread-safety audit against the other marked systems (no GL/GPU calls, no EnTT structural changes, no `GameplayEventBus` publish, no draws from the seeded game-thread RNG stream #452), recorded in the audit-table comment at the schedule builder. Unmarked systems are join-all barriers, so a marked system only ever overlaps other marked systems; edges between marked systems become task prerequisites; everything joins before the tick returns. Debug/bisect lever for BOTH mechanisms: `OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1` (env) or `SystemScheduler::SetParallelExecutionEnabled(false)` — same systems, same order, one thread (the kick then steps physics synchronously too). The editor Simulate-mode tick keeps the synchronous `Scene::StepPhysics`, which shares the same `JoltScene` phases + `PostPhysicsSync` so the two paths cannot drift.
- **EnTT + worker threads: first-touch is a WRITE.** `registry.view<T>()` / `registry.group<…>()` lazily **create** missing storage — a structural mutation of the registry's pool map — and entt's global `type_index` counter is a plain integer unless `ENTT_USE_ATOMIC` is defined. Two worker tasks first-touching component types concurrently can race the pool map and hand two types the same global id, corrupting the type→pool mapping **process-wide** — surfacing much later, in an unrelated single-threaded test, as the entt `"Unexpected type"` assert (this exact failure crashed the full suite during the #453 parallel slice: headless scenes never run `InitAudioRuntime`, so the audio groups' first-touch happened on a worker). Both guards are now in place and must be kept: `ENTT_USE_ATOMIC` is a **PUBLIC** compile definition on the `OloEngine` target (every TU including entt.hpp must agree or the `ENTT_MAYBE_ATOMIC` inline definitions violate ODR), and `Scene::Scene()` **pre-creates** every storage/group the `Parallelizable` systems touch. When marking another system `Parallelizable()`, extend that constructor pre-warm list with every component type its views/groups touch.
- **Stateless layered render command queue** (Molecular Matters style) — queue population is separated from execution.
- **Asset system:** `AssetManager::LoadAssetFromFile()` returns a handle; retrieve typed assets via `GetAsset<T>()`. Hot-reload via filewatch fires `AssetReloadedEvent`. Adding a new asset type means loader + `AssetManager` registration + YAML serialization + hot-reload handling.
- **Smart pointers / primitives:** `Ref<T>` from `Core/Ref.h`; integer / float typedefs (`u32`, `f32`, `sizet`, …) from `Core/Base.h`.
- **Profiling:** wrap with `OLO_PROFILE_FUNCTION()` / `OLO_PROFILE_SCOPE("name")` (Tracy). Use `RendererProfiler` and `RendererMemoryTracker` for renderer-specific metrics.
- **Physics:** Jolt (3D, `scene->GetPhysicsScene()`) + Box2D (2D), with custom collision layers.

### OloHeaderTool — generated scripting bindings + AllComponents tuple + SaveGame lists

[tools/OloHeaderTool/](tools/OloHeaderTool/) scans `OloEngine/src/` and emits:

- C++ binding glue (from `OLO_PROPERTY` annotations) → `OloEngine/src/OloEngine/Scripting/C#/Generated/`
- C# bindings (from `OLO_PROPERTY` annotations) → `OloEngine-ScriptCore/src/OloEngine/`
- the ECS `AllComponents` type list (from every `struct *Component` *definition*, minus a small runtime-only exclusion set) → `OloEngine/src/OloEngine/Scene/Generated/AllComponents.Generated.inl`, `#include`d by `Scene/Components.h`. This collapses one of the formerly-hand-maintained ECS component touch-points into codegen (issue #380, first slice).
- the SaveGame `SAVE_COMPONENT(...)` / `TRY_LOAD_COMPONENT(...)` enumeration lists (same `struct *Component` scan, minus a save-game-specific exclusion set `kComponentsNotInSaveGame` — components with no save serializer) → `OloEngine/src/OloEngine/SaveGame/Generated/SaveGameComponent{Capture,Restore}.Generated.inl`, `#include`d by `SaveGame/SaveGameSerializer.cpp`. This collapses the two most dangerous unguarded ECS touch-points — a component missing from either list was *silently* dropped from every save-game (issue #380, second slice).
- the Scene `OnComponentAdded<T>` / `OnComponentRemoved<T>` no-op specialization lists (same `struct *Component` scan, minus two *distinct* custom-handler exclusion sets `kComponentsCustomOnAdd` / `kComponentsCustomOnRemove` — the components whose add/remove callback is hand-written because it does real init/teardown) → `OloEngine/src/OloEngine/Scene/Generated/OnComponent{Added,Removed}.Generated.inl` (same `Scene/Generated` dir as the tuple, no extra CLI arg), `#include`d by `Scene/Scene.cpp` inside the `OLO_ON_COMPONENT_{ADDED,REMOVED}_NOOP` macros. This collapses the `OnComponentAdded`/`OnComponentRemoved` touch-point (issue #380, third slice); the declaration-only primary templates mean a mis-excluded handler is a loud build error (duplicate-definition or unresolved-symbol), never silent.
- the editor's **MCP writable-field registry** (issue #607) → `OloEditor/src/MCP/Generated/McpFieldRegistry.Generated.inl`, `#include`d by `OloEditor/src/MCP/McpGenericFieldWrite.h` inside `BuildRegistry()`. Driven by the same `CollectComponentFields` scan as the scene serializer, but with a *different* consumer contract: one `MakeField<Comp>(…, &Comp::Member)` entry per **public, JSON-coercible** member (bool / int / uint / small int / float / `glm::vec2|3|4` / enum / `std::string` / `AssetHandle`) of every component, minus `kComponentsNotMcpEditable` (a **fifth** exclusion set — the per-tick `*StateComponent` family, `AnimationStateComponent`, `UIResolvedRectComponent`, `WorldTransformComponent`, `IDComponent`). Fields with a serializer-enforced range (`OLO_SERIALIZE(Clamp, …)`, or the generator's `kMcpFieldClamps` table for hand-written serializers) emit a ranged entry so an MCP write is clamped exactly like a scene load. Note this is the one generated artefact that lands under `OloEditor/` rather than `OloEngine/`; it is a *superset* consumer of the serializer's field scan (a component that stays hand-written in `SceneSerializer.cpp` still contributes its recognised public fields here, since `ComponentSerInfo::fields` is populated even when `trivial == false`). Guarded by `OloEngine/tests/MCP/McpFieldRegistryTest.cpp`.
- the Scene serializer per-component serialize/deserialize blocks → `OloEngine/src/OloEngine/Scene/Generated/Scene{Serialize,Deserialize}Components.Generated.inl` (same `Scene/Generated` dir, no extra CLI arg), `#include`d by `Scene/SceneSerializer.cpp`. Driven by a **separate full data-member scan** (`CollectComponentFields`), **not** the `OLO_PROPERTY` scan — the serializer persists *every* field, not just script-exposed ones (e.g. `DirectionalLightComponent` serializes 9 fields but annotates only 3). A component is emitted iff **every** data member is a recognised trivial type **and public**, and the component is not in `kComponentsCustomSerialize`; the per-field `OLO_SERIALIZE(Skip|Clamp|Reject)` annotations cover runtime-only fields and range validation without excluding the whole component. This collapsed the last big *unguarded* ECS touch-point (issue #380, fourth slice) — a forgotten field was **silent scene-data loss**, which the old name-only coverage test couldn't catch. Guarded by `ComponentSerializerCoverageTest` (existence + `NoComponentIsBothHandWrittenAndGenerated` disjointness). The eligibility rule, the three annotations, the exclusion roster, the coverage blind spot and the per-slice history live in [docs/agent-rules/component-serializer-codegen.md](docs/agent-rules/component-serializer-codegen.md) — read it before widening the classifier.

Wired as the `GenerateBindings` target; `OloEngine` and `OloEngine-ScriptCore` depend on it, so it runs automatically on build. Since issue #758 it is a depfile-gated `add_custom_command`, so it re-runs only when a scanned header changed rather than on every build. If you change an annotated property, add/rename a component, and the generated `.inl` / `.cs` look stale, build `GenerateBindings` directly (then re-stage the regenerated files — they're tracked, not git-ignored); `--target codegen` does not exist here. **Before changing that rule, read [docs/agent-rules/build-trees-and-windows-asan.md](docs/agent-rules/build-trees-and-windows-asan.md) §1b** — it covers the depfile/stamp design, why `CODEGEN` is unused, how to force a regeneration, and four silent generator-specific traps. **Gotcha for future codegen slices:** the coverage tests that guard these touch-points parse generated/source files as *text* (e.g. `ComponentTupleCoverageTest::CollectTupleMembers` reads the `AllComponents = ComponentGroup<…>` marker) — when you move a touch-point from a hand-written location into a generated file, repoint the test's parser at the generated path or it fails on an empty parse.

### Editor undo/redo for components

[OloEditor/src/Panels/SceneHierarchyPanel.cpp](OloEditor/src/Panels/SceneHierarchyPanel.cpp)`::DrawComponent<T>` uses a three-tier `constexpr if`:

1. `std::is_trivially_copyable_v<T>` → byte-level `memcmp`.
2. `std::equality_comparable<T>` → copy-before / copy-after with `operator==`.
3. fallback → no undo.

To opt a non-trivially-copyable component into undo, give it `auto operator==(const T&) const -> bool = default;` (the trailing-return form — MSVC rejects plain `auto = default;`). See [docs/agent-rules/cpp-coding-quality.md](docs/agent-rules/cpp-coding-quality.md) §7 for the MSVC quirk and the `UUID` C2666 footgun.

---

## Conventions

- C++23 (`CMAKE_CXX_STANDARD = 23`), 4-space indent, braces on new lines except trivial cases.
- Naming: classes `PascalCase`, members `m_PascalCase`, statics `s_PascalCase`.
- Project headers `#include "..."`, third-party / system `#include <...>`. PCH is `OloEnginePCH.h`; public headers must be self-contained (Include What You Use).
- `#pragma once` for header guards.
- Floating-point: never `==` / `!=` on `float`/`double`/`glm::vec*`/`glm::mat*` — see [docs/agent-rules/cpp-coding-quality.md](docs/agent-rules/cpp-coding-quality.md) §2. Validate any float read from YAML/JSON/network with `std::isfinite`.

Full coding rules in [docs/agent-rules/cpp-coding-quality.md](docs/agent-rules/cpp-coding-quality.md).

---

## Common pitfalls

- **Wrong working directory** → missing shaders / Mono assemblies at startup.
- **Editing under `OloEngine/vendor/`** → wiped on next CMake reconfigure. Since issue #773 that directory only holds the *remaining* in-tree deps (imgui, glad, lua, sol2, stb, filewatch, …); everything vcpkg provides lives in `<buildDir>/vcpkg_installed/`, which is equally not-yours-to-edit — patch a port with an overlay in `cmake/overlay-ports/` instead, and keep it a minimal diff against `$VCPKG_ROOT/ports/<name>/`.
- **Building the msvc (`build/`) and clangcl (`build-clang/`) trees at the same time** → sequence them; check for live `MSBuild`/`ninja` processes (including background builds from an earlier turn) before starting either. Since issue #758 the shared-generated-file collision is no longer a *lock* failure or a half-written `.inl`, and since #773 the two trees no longer share a vendor build directory (each gets its own `vcpkg_installed/`). They deliberately share the **same** `x64-windows-static-md` target triplet, and therefore the same binary-cache entries — the ports are built once, with cl.exe, for both — so the two installed trees hold identical packages rather than differing flavors. But `mspdbsrv` (per-USER) and the memory ceiling still make it a rule. A `vcpkg install` is a *third* concurrent compile source with its own `VCPKG_MAX_CONCURRENCY`; count it against the same budget. See [docs/agent-rules/build-trees-and-windows-asan.md](docs/agent-rules/build-trees-and-windows-asan.md) §1.
- **Assuming a vcpkg port carries the version we pin** → seven of the #773 audit's "clean moves" were not. Three were later taken anyway at an older registry version, deliberately (entt v4→3.16, glm master→1.0.3, tracy 0.14.0→0.13.1); four still stand (stb is ~2 years behind on an image decoder, imguizmo's port would link a second Dear ImGui, sol2+lua are a tested pair whose port versions mean adopting Lua 5.5). Check the registry version against the current pin before moving anything, and weigh a header-only dep's near-zero cache benefit against the version delta. See [docs/agent-rules/vcpkg-dependency-management.md](docs/agent-rules/vcpkg-dependency-management.md).
- **Adding a component without updating every hand-maintained touch-point** (`SaveGameComponentSerializer.{h,cpp}` — `Serialize()` overload + `RegisterAll()`, `LuaScriptGlue.cpp::RegisterAllTypes`, `OLO_PROPERTY` annotations, and — only for **non-trivial-field** components — a hand-written `SceneSerializer.cpp` block) → scenes don't persist, save-games drop the component, scripts break silently. The pre-commit hook can't catch this; see the Definition of done above. (The `AllComponents` tuple, the SaveGame `SAVE_COMPONENT`/`TRY_LOAD_COMPONENT` enumeration lists, the `Scene::OnComponentAdded<T>`/`OnComponentRemoved<T>` no-op specializations, **and** the `SceneSerializer.cpp` serialize/deserialize blocks for **all-trivial** components are **no longer** hand-maintained — OloHeaderTool generates all four from the component definitions. A plain all-trivial component now needs **zero** edits in `SceneSerializer.cpp` and round-trips through scene YAML automatically; you only hand-write a serializer block when the component has a non-trivial field — the parser skips it automatically — or needs custom deserialize logic, in which case you also add a `kComponentsCustomSerialize` exclusion (forgetting it ⇒ a loud double-emit test failure, not a silent drop). A plain no-op component needs **zero** edits in `Scene.cpp`; you only edit it (and add a `kComponentsCustomOnAdd`/`kComponentsCustomOnRemove` exclusion) when its add/remove callback needs a real body — that path fails loudly via the declaration-only primary template. Only a component lacking a save `Serialize()` overload needs a `kComponentsNotInSaveGame` exclusion edit, and only a runtime-only component needs a `kComponentsNotInTuple` exclusion edit.)
- **Naming a non-ECS helper struct `*Component`** → OloHeaderTool's `struct ...Component` scan sweeps it into the generated `AllComponents` / SaveGame / `OnComponentAdded` / SceneSerializer `.inl`, and the build fails with `error C2065: '<YourStruct>': undeclared identifier` (and a `C3544` parameter-pack error) compiled from unrelated TUs. Any struct whose **name ends in "Component"** is matched, ECS or not. Give registry records / DTO helpers a different suffix (e.g. `InterpolationEntry`, `SnapshotComponentData`); the fix is rename + rebuild `GenerateBindings` to drop the stale generated entries.
- **Adding a test `.cpp` anywhere under `OloEngine/tests/` without classifying it** → pre-commit hook blocks the commit. The whole tree is scanned (no allowlist, no exclude list); every file with a `TEST`/`TEST_F`/`TEST_P`/`TYPED_TEST` macro must be classified (`L1`–`L11`/`plumbing`/… for renderer-scope, `Functional`, or `unit`). **Preferred:** add a `// OLO_TEST_LAYER: <id>` comment near the top of the test file — the classification then lives in the test file, so adding a test touches no shared file and two branches can't collide on it. Fallback: an entry in `test_catalogue.json` → `file_layer_map` (a file uses one or the other, not both). Fast inner loop: `run-fast-tests-debug` VS Code task, or `generate_test_catalogue.py --gtest-filter --exclude L6,L7,L8` to skip perf/golden/visual.
- **Hand-editing the generated catalogue docs (`docs/test-catalogue.*.md`)** → they're git-ignored and overwritten on every regenerate; classify via the in-file `// OLO_TEST_LAYER` tag or `test_catalogue.json` instead.
- **Using golden images as the *primary* correctness check** → the testing rules require an L1–L5 contract test as well.
- **A targeted `--target OloEditor` build silently ships without working C# scripting if the `OloEditor → OloEngine-ScriptCore`/`Sandbox-Scripting` dependency edge (`CMakeLists.txt`, next to the `OloRuntime` edge, guarded by the same `if(CMAKE_GENERATOR MATCHES "Visual Studio")` block) is ever removed** — `OloEngine-ScriptCore.dll` builds straight into `OloEditor/Resources/Scripts/`; without that edge the DLL never gets built, `ScriptEngine::Init` fails to `LoadAssembly` it, logs a warning, and disables C# scripting for the session (graceful degradation, not a crash — easy to miss in an agent visual-verification loop that only checks for a rendered window, not the log). If you ever see `[ScriptEngine] OloEngine-ScriptCore assembly unavailable` in `OloEngine.log`, check that edge first before re-diagnosing from scratch (issue discovered/fixed via a `/start-work` runtime smoke-test, not a tracked GitHub issue).

---

## Agent skills

- **Issue tracker** — GitHub Issues at `drsnuggles8/OloEngineBase` via the `gh` CLI.
- **ADRs** — architecture decisions live in `docs/adr/`. Read existing ones before proposing structural changes.
