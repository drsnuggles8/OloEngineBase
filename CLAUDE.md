# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Companion guides

This repo already ships extensive agent guidance — read these before doing anything non-trivial; do not duplicate their content here:

- `.github/copilot-instructions.md` — concise architectural overview, conventions, and adding-a-system checklist.
- `.github/instructions/cpp-coding-quality.instructions.md` — SonarQube-aligned C++ rules (init-statements, float comparison, `auto`, loop hoisting, IWYU, defaulted `operator==` MSVC quirk).
- `.github/instructions/testing-architecture.instructions.md` — the renderer 11-layer pyramid, the Functional/cross-subsystem axis, and the registration contract.
- `.github/instructions/glsl-shaders.instructions.md`, `cpp-language-service-tools.instructions.md` — shader and tooling rules.
- `docs/BUILD.md` — full Windows / Linux / WSL build matrix.
- `docs/testing.md` — the opinion document: WHY we test what we test, the value heuristic, named anti-patterns with concrete file examples, retirement criteria.
- `docs/testing.md` — authoritative renderer-testing reference (L1–L11; auto-catalogue lives here).
- `docs/testing.md` — authoritative Functional / cross-subsystem testing reference (`Scene::OnUpdateRuntime` seams; separate auto-catalogue).

## Build & run

CMake presets (`CMakePresets.json`):

- `msvc` (Visual Studio 18 2026, `build/`) — primary; default build dir.
- `clangcl` (Ninja Multi-Config, `build-clang/`) — clang-cl warnings with MSVC ABI.
- `clangcl-asan` — adds AddressSanitizer.

Common commands (run from repo root):

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

VS Code tasks (`.vscode/tasks.json`) wrap the above: `build-oloeditor-debug`, `run-oloeditor-debug`, `build-tests-debug`, `run-tests-debug`, `build-clangcl-tests-debug`, `configure-clangcl`, etc.

**Working directory matters.** `OloEditor`, `OloRuntime`, and `OloServer` resolve assets, shaders, and Mono assemblies relative to `OloEditor/`. Always run with `cwd = OloEditor/` (the VS Code tasks already do this; the test binary instead runs from repo root).

Targets: `OloEngine` (static lib), `OloEditor`, `OloRuntime`, `OloServer`, `OloEngine-Tests`, plus `OloEngine-LuaScriptCore` and (Visual Studio generator only) `OloEngine-ScriptCore` for C#. `OloEditor` `add_dependencies` on `OloRuntime` so the BuildGamePanel never ships a stale runtime. `OloEngine` and `OloEngine-ScriptCore` both depend on the `GenerateBindings` custom target — see "OloHeaderTool" below.

## Tests

GoogleTest, registered via `OloEngine/tests/CMakeLists.txt`. Run with the `run-tests-debug` task or:

```powershell
build\OloEngine\tests\Debug\OloEngine-Tests.exe
build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=SuiteName.TestName
```

**Two test axes + registration contract.** OloEngine tests live on two independent axes that share only GoogleTest and the registration hook:

1. **Renderer testing pyramid** (L1–L11 + plumbing/cullinglod/shaderpipe/integration/meta) — pixels, GL state, shader math.
2. **Functional / cross-subsystem axis** (tag `"Functional"`) — Animation × Physics × Scripting × Networking × Audio × Asset × Nav × Save-game × Gameplay × AI seams driven by real `Scene::OnUpdateRuntime`.

Both axes (philosophy, anti-patterns, decision tree, per-layer reference) are documented in `docs/testing.md`.

Every `.cpp` under `OloEngine/tests/{Rendering,ShaderGraph,Streaming,Functional}/` (and `extra_roots`) must appear in `OloEngine/tests/scripts/test_catalogue.json` (`file_layer_map` for tests, `exclude` for helpers). The `test-catalogue-in-sync` pre-commit hook fails commits that violate this. After adding/removing a test file:

```powershell
python OloEngine\tests\scripts\generate_test_catalogue.py
```

That rewrites both auto-catalogue blocks inside `docs/testing.md` (between the `<!-- BEGIN: renderer-catalogue ... -->` / `<!-- END: renderer-catalogue -->` and `<!-- BEGIN: functional-catalogue ... -->` / `<!-- END: functional-catalogue -->` marker pairs). Never hand-edit those blocks.

When adding an engine feature, pick the right classification(s): L1 property (math contracts), L2 shader unit (compute readback), L3 round-trip (CPU↔GPU), L4 GL state (`GLStateGuard`), L5 hazard (render graph), L6 perf (with `perf_baselines.txt` entry — use the anti-flake retry helper, never raw `EXPECT_LT` on timings), L7 smoke (NaN/Inf), L8 golden (SSIM, only after L1–L4 pin the contract), L11 fuzz (anything parsing external data), `"Functional"` (anything that drives a cross-subsystem seam through `Scene::OnUpdateRuntime`). The decision tree is in the testing-architecture instructions file.

Special rebase modes: `OLOENGINE_GOLDEN_REBASE=1` for goldens, `OLOENGINE_PERF_REBASE=1` for perf baselines — only after a deliberate visual change or hardware/optimisation move.

## Architecture (high-level)

C++23 baseline, OpenGL 4.6 with DSA. Layout:

- `OloEngine/src/OloEngine/<Subsystem>/` — engine library. Subsystems: `Renderer`, `Scene`, `Physics3D`, `Asset`, `Audio`, `Animation`, `Scripting`, `Networking`, `UI`, `Particle`, `Navigation`, `Precipitation`, `Snow`, `Wind`, `Terrain`, `Dialogue`, `SaveGame`, `Server`, `AI`, `Gameplay`, plus core `Core`, `Memory`, `Threading`, `Task`, `Async`, `Containers`, `Templates`, `Math`, `Events`, `Serialization`, `Project`, `Build`, `Debug`, `Misc`, `Utils`, `Platform`, `Algo`, `HAL`, `ImGui`.
- `OloEditor/` — ImGui editor. Panels live under `src/`; runtime assets under `assets/`; sample game under `SandboxProject/`.
- `OloRuntime/` — standalone game runtime that loads what the editor builds.
- `OloServer/` — headless dedicated server (the only target that runs on WSL2).
- `OloEngine-ScriptCore/` (C# / Mono, Windows only) and `OloEngine-LuaScriptCore/` (Lua / Sol2, all platforms).
- `OloEngine/vendor/` — `FetchContent` / CPM downloads. **Never edit; CMake reconfigure wipes changes.**

Cross-cutting patterns to know:

- **ECS:** EnTT under an `Entity` wrapper (UUID). Components are POD-ish; serialization lives in `Core/YAMLConverters.h`.
- **Stateless layered render command queue** (Molecular Matters style) — queue population is separated from execution.
- **Asset system:** `AssetManager::LoadAssetFromFile()` returns a handle; retrieve typed assets via `GetAsset<T>()`. Hot-reload via filewatch fires `AssetReloadedEvent`. Adding a new asset type means: loader + `AssetManager` registration + YAML serialization + hot-reload handling.
- **Smart pointers / primitives:** `Ref<T>` from `Core/Ref.h`; integer/float typedefs (`u32`, `f32`, `sizet`, …) from `Core/Base.h`.
- **Profiling:** wrap with `OLO_PROFILE_FUNCTION()` / `OLO_PROFILE_SCOPE("name")` (Tracy). Use `RendererProfiler` and `RendererMemoryTracker` for renderer-specific metrics.
- **Physics:** Jolt (3D, `scene->GetPhysicsScene()`) + Box2D (2D), with custom collision layers.

### OloHeaderTool — generated scripting bindings

`tools/OloHeaderTool/` scans `OloEngine/src/` for `OLO_PROPERTY` annotations and emits:

- C++ binding glue → `OloEngine/src/OloEngine/Scripting/C#/Generated/`
- C# bindings → `OloEngine-ScriptCore/src/OloEngine/`

Wired as the `GenerateBindings` custom target; `OloEngine` and `OloEngine-ScriptCore` depend on it, so it runs automatically on configure/build. If you change an annotated property and the generated `.inl` / `.cs` look stale, build `GenerateBindings` directly.

When adding/changing a component that scripting will see, update **both** the C# (ScriptCore) and Lua (LuaScriptCore) bindings plus serialization — missing one of the three causes silent script/scene failures.

### Editor undo/redo for components

`SceneHierarchyPanel.cpp::DrawComponent<T>` uses a three-tier `constexpr if`:

1. `std::is_trivially_copyable_v<T>` → byte-level `memcmp`.
2. `std::equality_comparable<T>` → copy-before/copy-after with `operator==`.
3. fallback → no undo.

To opt a non-trivially-copyable component into undo, give it `auto operator==(const T&) const -> bool = default;` (the trailing-return form — MSVC rejects plain `auto = default;`). `UUID` has an implicit `operator u64()` that causes C2666 ambiguity with member `operator==` — compare via `static_cast<u64>()` in manual implementations. Floats inside components must use `std::memcmp` per the C++ coding-quality rules.

## Conventions

- C++23 (`CMAKE_CXX_STANDARD = 23`), 4-space indent, braces on new lines except trivial cases.
- Naming: classes PascalCase, members `m_PascalCase`, statics `s_PascalCase`.
- Project headers `#include "..."`, third-party / system `#include <...>`. PCH is `OloEnginePCH.h` — but public headers must be self-contained (Include What You Use; see coding-quality rules §5).
- `#pragma once` for header guards.
- Floating-point: never `==` / `!=` on `float`/`double`/`glm::vec*`/`glm::mat*` — use epsilon for tolerance, `std::memcmp` for bitwise (coding-quality rules §2). Validate any float read from YAML/JSON/network with `std::isfinite`.

## Pre-commit

```powershell
python -m pip install --user pre-commit
pre-commit install                    # one-time
pre-commit run --all-files            # before opening a PR
```

Hooks: `trailing-whitespace`, `end-of-file-fixer`, `check-yaml`, `clang-format` (`.clang-format` at repo root), and the `test-catalogue-in-sync` python script. The config excludes `vendor/`, `mono/`, `.vs/`, `.vscode/`, `build/`, `bin/`, `.idea/`, and `.github/workflows/` — never run formatters across vendor trees. Run pre-commit once after finishing changes; a second run is unnecessary, and the result is formatting-only so no rebuild is needed.

## Common pitfalls

- Wrong working directory → missing shaders / Mono assemblies at startup.
- Editing under `OloEngine/vendor/` → wiped on next CMake reconfigure.
- Adding a component without updating C# bindings + Lua bindings + YAML serialization → scenes don't persist or scripts break silently.
- Adding a `.cpp` under the test scan roots without registering it in `test_catalogue.json` → pre-commit hook blocks the commit.
- Hand-editing the auto-catalogue blocks in `docs/testing.md` → overwritten on next regenerate.
- Using golden images as the *primary* correctness check → the testing rules require an L1–L5 contract test as well.

## Agent skills

### Issue tracker

GitHub Issues at `drsnuggles8/OloEngineBase` via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Mapped onto the existing label set: `needs-info`→`question`, `ready-for-human`→`help wanted`, `wontfix`→`wontfix`. `needs-triage` and `ready-for-agent` are created on first use. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: one `CONTEXT.md` + `docs/adr/` at the repo root. Created lazily by `/grill-with-docs`. See `docs/agents/domain.md`.
