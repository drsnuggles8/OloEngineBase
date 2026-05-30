# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## Don't commit on your own initiative — ask first

**Never run `git commit`, `git commit --amend`, `git push`, `git push --force`, `git merge`, `git rebase`, `git reset --hard`, or `gh pr create` without an explicit instruction.** "Commit this", "push the branch", "open a PR" are clear go-aheads. Anything less direct — "we're done", "looks good", "wrap it up", a `/done` closing, or your own judgement that the work is in a good state — is not.

What you may always do without asking: edit files, create branches with `git checkout -b`, stage with `git add`, run `git status` / `git diff` / `git log`, and report what's ready.

**When work reaches a finishable state — pre-commit clean, tests pass, scope boundary obvious — ask the user whether to commit before pivoting to something else.** A one-line offer like "Want me to commit these N changes as `<draft message>`?" is enough. Skip the ask only when the user has just told you they want to keep iterating, or when the state isn't actually finishable.

The commit itself is still a deliberate opt-in; this is just removing the "silently sit on a clean tree" default that can block a clean branch switch later.

---

## Definition of done — before you hand back to the user

When you finish a task that touched code or assets:

1. **Pre-commit is automatic.** A `Stop` hook in `.claude/settings.json` runs `pre-commit run --all-files` at the end of every turn (the wrapper script at `scripts/claude-stop-hook.ps1` is the source of truth — it runs the whole repo, not just modified files, on purpose). If it reformats anything, the changes are already on disk — re-stage them before committing. Do **not** run `pre-commit` again manually unless it failed.
2. **Test-catalogue regeneration** is enforced by the `test-catalogue-in-sync` hook above. If it fails, run `python OloEngine/tests/scripts/generate_test_catalogue.py` and re-stage.
3. **Cross-binding check** — the pre-commit hook does **not** catch this. If you added or changed an ECS component, you must update **all five** touch-points or scripting / scene saves / save-games / runtime copies will silently drop it:
   - `Scene/Components.h` — the component struct itself **and** add it to the `AllComponents` tuple at the bottom of the file (scene copy, prefab instantiation, and `HasComponent<T>()` script registration all iterate this tuple).
   - `Scene/SceneSerializer.cpp` — `SerializeEntity` (writes) and `DeserializeEntities` (reads). Validate every float with `std::isfinite`.
   - `SaveGame/SaveGameComponentSerializer.{h,cpp}` — forward decl, `Serialize()` overload, and `RegisterAll()` registration. Save-games will not round-trip otherwise.
   - `Scripting/C#/Generated/` is auto-generated from `OLO_PROPERTY` annotations by OloHeaderTool (see below) — add the annotations on the component fields. Don't hand-edit the generated `.inl` / `.cs`.
   - `Scripting/Lua/LuaScriptGlue.cpp::RegisterAllTypes()` — Sol2 usertype registration. (`OloEngine-LuaScriptCore/` is the Mono-equivalent project target but the actual bindings live in `Scripting/Lua/`.)

   Missing any one causes silent script or scene failures. Audit the existing list in `AllComponents` (`Scene/Components.h`, near the bottom of the file) as the source of truth for what's expected to round-trip.

If you find yourself wanting to write "remember to run pre-commit" anywhere, don't — the hook owns that.

---

## Companion guides

Read before doing anything non-trivial; do not duplicate their content here:

- [docs/agent-rules/cpp-coding-quality.md](docs/agent-rules/cpp-coding-quality.md) — init-statements, float comparison, `auto`, loop hoisting, IWYU, defaulted `operator==` MSVC quirk.
- [docs/agent-rules/testing-architecture.md](docs/agent-rules/testing-architecture.md) — renderer 11-layer pyramid, Functional/cross-subsystem axis, registration contract.
- [docs/agent-rules/glsl-shaders.md](docs/agent-rules/glsl-shaders.md) — SPIR-V constraints, UBO bindings, MRT output.
- [docs/BUILD.md](docs/BUILD.md) — full Windows / Linux / WSL build matrix.
- [docs/testing.md](docs/testing.md) — opinion document on **why** we test what we test, value heuristic, named anti-patterns, retirement criteria, and the auto-catalogues for both test axes.

---

## Build & run

CMake presets ([CMakePresets.json](CMakePresets.json)) — note all three require **CMake 4.2+** because the `msvc` preset's `Visual Studio 18 2026` generator only exists in CMake 4.2 and the others inherit / share the version requirement at the top of the file:

- `msvc` (Visual Studio 18 2026, `build/`) — primary; default build dir.
- `clangcl` (Ninja Multi-Config, `build-clang/`) — clang-cl warnings with MSVC ABI.
- `clangcl-asan` — adds AddressSanitizer.

```powershell
# Generate VS solution
scripts\Win-GenerateProjectVS2022.bat   # or VS2026

# Build a target
cmake --build build --target OloEditor       --config Debug --parallel
cmake --build build --target OloEngine-Tests --config Debug --parallel
cmake --build build --target OloRuntime      --config Debug --parallel
cmake --build build --target OloServer       --config Debug --parallel

# ClangCL (configure once, then build)
cmake --preset clangcl
cmake --build build-clang --target OloEngine-Tests --config Debug --parallel
```

VS Code tasks ([.vscode/tasks.json](.vscode/tasks.json)) wrap the above: `build-oloeditor-debug`, `run-oloeditor-debug`, `build-tests-debug`, `run-tests-debug`, `build-clangcl-tests-debug`, `configure-clangcl`, etc.

**Working directory matters.** `OloEditor`, `OloRuntime`, and `OloServer` resolve assets, shaders, and Mono assemblies relative to `OloEditor/`. Always run with `cwd = OloEditor/` (the VS Code tasks already do this; the test binary runs from repo root instead).

Targets: `OloEngine` (static lib), `OloEditor`, `OloRuntime`, `OloServer`, `OloEngine-Tests`, plus `OloEngine-LuaScriptCore` and (Visual Studio generator only) `OloEngine-ScriptCore` for C#. `OloEditor` `add_dependencies` on `OloRuntime` so the BuildGamePanel never ships a stale runtime. `OloEngine` and `OloEngine-ScriptCore` both depend on the `GenerateBindings` custom target — see "OloHeaderTool" below.

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

Special rebase modes: `OLOENGINE_GOLDEN_REBASE=1` for goldens, `OLOENGINE_PERF_REBASE=1` for perf baselines — only after a deliberate visual change or a hardware/optimisation move.

### Rendering changes MUST be visually verified — unit tests are not enough

A renderer change can pass every CPU/contract test and still look completely broken on screen (transparent water, foam wash, fog flooding the frame, z-fighting, a hard seam at the waterline). **Math/contract tests prove the formula; they do not prove the frame looks right.** So for any change that affects what the screen shows — shaders, render passes, materials, post-processing, culling, blending, a new visual component — do **all** of:

1. **Pin the math with CPU/contract tests** (the cheapest layer that proves the formula) — e.g. the fog/opacity/face-selection logic in [WaterRenderingTest.cpp](OloEngine/tests/Rendering/WaterRenderingTest.cpp). These run in CI.
2. **Capture screenshot evidence from multiple camera angles and actually look at the pixels.** Follow the visual-probe pattern in [SphereAreaLightVisualTest.cpp](OloEngine/tests/Rendering/PropertyTests/SphereAreaLightVisualTest.cpp) and [WaterVisualEvidenceTest.cpp](OloEngine/tests/Rendering/PropertyTests/WaterVisualEvidenceTest.cpp): render the real pipeline to a framebuffer, read it back, `stbi_write_png` to `OloEditor/assets/tests/visual/`, and **read the PNGs back** to confirm the result. Cover the cases where the effect is most likely to break — for water that meant *from the side*, *straddling the waterline*, *fully submerged*, and *looking down from above*. Don't trust "the test passed"; trust the image.
3. **Run the change in the editor** (`run-oloeditor-debug`, scene under `OloEditor/SandboxProject/Assets/Scenes/`) and check the `OloEngine.log` for shader compile/link errors. Add a representative test scene if one doesn't exist.

Do not report a rendering change as done on the strength of unit tests alone — that produced multiple "tests green, screen wrong" rounds. If you cannot capture/inspect a frame for a given change, say so explicitly.

**Visual-regression tests now run in the suite (issue #258, mostly closed):** the `RendererAttachedTest` fixture's `SetRenderingEnabled(false)` default is now an *opt-out*, not a hard limit. Call `EnableRendering(w, h)` and drive the render through a guarded helper — `RunFrames` (runtime primary camera) or `RunEditorFrames` (an explicitly posed `EditorCamera`, for multi-angle screenshots). Each wraps the full-pipeline tick in a `GLStateGuard(Restore)`, so a render no longer poisons later GPU tests; a process-wide `Renderer::Shutdown()` after `RUN_ALL_TESTS()` fixes the old teardown SIGSEGV. With that, `SceneRenderEvidenceTest` (lit cube) and `WaterVisualEvidenceTest` (water from 6 angles, golden-PNG + driver-independent contracts) run in the normal suite and **SKIP cleanly** (not fail) when no GL 4.6 context exists — so they gate any GPU-equipped run while headless CI skips them. New screenshot tests should follow this pattern instead of `DISABLED_`. Still open under #258: `AssetSceneLoadTest.DISABLED_` (a separate deserializer-refactor root cause, not GL-state hygiene).

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

- **ECS:** EnTT under an `Entity` wrapper (UUID). Components are POD-ish; serialization lives in [OloEngine/src/OloEngine/Core/YAMLConverters.h](OloEngine/src/OloEngine/Core/YAMLConverters.h).
- **Stateless layered render command queue** (Molecular Matters style) — queue population is separated from execution.
- **Asset system:** `AssetManager::LoadAssetFromFile()` returns a handle; retrieve typed assets via `GetAsset<T>()`. Hot-reload via filewatch fires `AssetReloadedEvent`. Adding a new asset type means loader + `AssetManager` registration + YAML serialization + hot-reload handling.
- **Smart pointers / primitives:** `Ref<T>` from `Core/Ref.h`; integer / float typedefs (`u32`, `f32`, `sizet`, …) from `Core/Base.h`.
- **Profiling:** wrap with `OLO_PROFILE_FUNCTION()` / `OLO_PROFILE_SCOPE("name")` (Tracy). Use `RendererProfiler` and `RendererMemoryTracker` for renderer-specific metrics.
- **Physics:** Jolt (3D, `scene->GetPhysicsScene()`) + Box2D (2D), with custom collision layers.

### OloHeaderTool — generated scripting bindings

[tools/OloHeaderTool/](tools/OloHeaderTool/) scans `OloEngine/src/` for `OLO_PROPERTY` annotations and emits:

- C++ binding glue → `OloEngine/src/OloEngine/Scripting/C#/Generated/`
- C# bindings → `OloEngine-ScriptCore/src/OloEngine/`

Wired as the `GenerateBindings` custom target; `OloEngine` and `OloEngine-ScriptCore` depend on it, so it runs automatically on configure/build. If you change an annotated property and the generated `.inl` / `.cs` look stale, build `GenerateBindings` directly.

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
- **Editing under `OloEngine/vendor/`** → wiped on next CMake reconfigure.
- **Adding a component without updating all five touch-points** (`AllComponents` tuple, `SceneSerializer.cpp`, `SaveGameComponentSerializer.{h,cpp}`, `LuaScriptGlue.cpp::RegisterAllTypes`, `OLO_PROPERTY` annotations) → scenes don't persist, save-games drop the component, scene copy / prefab instantiation silently skips it, scripts break silently. The pre-commit hook can't catch this; see the Definition of done above.
- **Adding a `.cpp` under the test scan roots without registering it in `test_catalogue.json`** → pre-commit hook blocks the commit.
- **Hand-editing the auto-catalogue blocks in [docs/testing.md](docs/testing.md)** → overwritten on next regenerate.
- **Using golden images as the *primary* correctness check** → the testing rules require an L1–L5 contract test as well.

---

## Agent skills

- **Issue tracker** — GitHub Issues at `drsnuggles8/OloEngineBase` via the `gh` CLI.
- **ADRs** — architecture decisions live in `docs/adr/`. Read existing ones before proposing structural changes.
