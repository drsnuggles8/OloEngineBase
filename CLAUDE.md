# CLAUDE.md

Rules for an agent working in this repository. Written for Claude Code, but tool-neutral apart from
the section *For other agents* at the end. Rules live here; the reasons and the stories behind them
live in the linked guides.

## Committing and publishing

Two contexts, different defaults.

**In a task worktree running the task loop** (a `feature/*` branch created by `/start-work`, session
started from `HANDOVER.md`): committing, pushing with `git push -u origin feature/<slug>`, opening
the PR and commenting on issues are pre-authorized. See
[docs/process/task-loop.md](docs/process/task-loop.md). Don't stop to ask at each step.

**Everywhere else** (the base repo, `master`, an ad-hoc session, a worktree with no handover): ask
first. "Commit this", "push the branch", "open a PR" are go-aheads; "we're done", "looks good" are
not. When work is finishable, offer once: "Want me to commit these N changes as `<message>`?"

**Gated in every context**, loop or not:

- committing or pushing to `master` directly
- `git push --force` / `--force-with-lease`
- `git reset --hard`, rebasing pushed commits, `git commit --amend` on a pushed commit
- merging a PR (`gh pr merge`)
- closing a GitHub issue: post the evidence, recommend closure, let the user close

**Never a bare `git push`.** Under `push.default` it can land on `master`, which has happened here.
Always `git push -u origin feature/<slug>`.

Always fine: editing files, `git checkout -b`, `git add`, `git status` / `diff` / `log`, additive
PR or issue comments.

## Scope

The request sets the scope, and the whole request is the deliverable. Three house rules on extras:

- **A bug you find on the way is fixed, not filed.** Fix it in its own commit on the same branch
  and list it in the PR body. File an issue only when the fix needs a decision from the user, needs
  access you lack, is bigger than the task itself, or touches files another live worktree owns,
  and say which in the issue. A wider diff is not a reason. Rules and issue format:
  [task-loop.md Phase 1a](docs/process/task-loop.md#1a-bugs-you-find-on-the-way--fix-them-here).
- **Tests** go where the task asks for them or where this repo already tests that kind of change,
  sized like the neighbouring test files. Scratch checks are not committed.
- **A new postmortem** (see *Writing docs here*) is welcome, but it is a separate file with a
  pointer, never a paragraph added to this one.

## Definition of done

When a task touched code or assets:

1. **Pre-commit runs itself.** A `Stop` hook runs `pre-commit run --all-files` at the end of every
   turn. If it reformatted anything, re-stage and commit again; a commit that triggers an auto-fix
   aborting is expected. Don't run `pre-commit` by hand unless it failed. A push in the same turn as
   the edits would race the hook, so a `PreToolUse` guard runs it first and blocks the push if the
   tree changes; re-stage its fixes and push again.
2. **Every test `.cpp` is classified.** Add `// OLO_TEST_LAYER: <id>` near the top (or a
   `file_layer_map` entry in `test_catalogue.json`, not both). The rendered catalogue under
   `docs/test-catalogue.*.md` is generated and git-ignored; never hand-edit it.
3. **Cross-binding check for ECS components.** Adding or changing a component has touch-points the
   pre-commit hook cannot catch: save-game `Serialize` + `RegisterAll`, Lua registration, the editor
   inspector and Add Component menu, and every field in a hand-written copy ctor / `operator==`.
   Before touching a `*Component`, read the table in
   [component-serializer-codegen.md §0](docs/agent-rules/component-serializer-codegen.md#0-adding-or-changing-a-component-every-touch-point).

## Companion guides

[docs/agent-rules/README.md](docs/agent-rules/README.md) indexes every guide by subsystem and by
failure mode. Read the relevant one before non-trivial work. Six are worth reading regardless of
subsystem:

- [cpp-coding-quality.md](docs/agent-rules/cpp-coding-quality.md): the coding rules.
- [glsl-shaders.md](docs/agent-rules/glsl-shaders.md): what makes a shader fail SPIR-V compile.
- [testing-architecture.md](docs/agent-rules/testing-architecture.md): where a new test belongs.
- [substituted-seams-compound.md](docs/agent-rules/substituted-seams-compound.md): every test substitution hides a seam.
- [build-trees-and-windows-asan.md](docs/agent-rules/build-trees-and-windows-asan.md): build trees, caches, memory.
- [sonarqube-review-alignment.md](docs/agent-rules/sonarqube-review-alignment.md): read before `/code-review`.

## Build & run

**`VCPKG_ROOT` must be set** (issue #773). Dependencies come from [vcpkg.json](vcpkg.json); a
configure without it stops at a guard. One-time setup and the traps:
[vcpkg-dependency-management.md](docs/agent-rules/vcpkg-dependency-management.md).

**`STEAMWORKS_SDK_ROOT` is optional** (licensed SDK, not in the repo; exactly one TU may include a
Valve header): [steamworks-platform-integration.md](docs/agent-rules/steamworks-platform-integration.md).

CMake presets ([CMakePresets.json](CMakePresets.json), all need CMake 4.2+):

| Preset | Dir | Use |
|---|---|---|
| `dev-cached` | `build-cached/` | **default.** clang-cl + compiler cache, shared across worktrees. |
| `msvc` | `build/` | what Visual Studio and the debugger use. Cannot cache, cannot build concurrently with anything. |
| `clangcl` / `clangcl-asan` | `build-clang/` | clang-cl warnings / AddressSanitizer. |

```powershell
cmake --preset dev-cached                       # once per worktree
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-cached --target OloEngine-Tests --config Debug --parallel 6'
```

**Every build goes through [build-lock.ps1](.claude/skills/run-oloengine/build-lock.ps1).** It
bounds concurrent builds across every worktree, sets the job count from free memory (so
`--parallel 6` is a hint it rewrites), and kills its build if the launching session dies. A
`PreToolUse` hook blocks a `cmake --build` / `ninja` / `msbuild` call that skips it. Two markers
opt out and are not interchangeable: `OLO_NOT_A_BUILD` for a command that only mentions a build
tool, `OLO_BUILD_LOCK_OVERRIDE` for a real unlocked build, which needs the user's permission for
that build and is audited. Why, and what it does not protect against:
[.claude/skills/run-oloengine/SKILL.md](.claude/skills/run-oloengine/SKILL.md).

A build **outside** the lock is never uncapped (`--parallel N` or `ninja -jN`; the box has been
OOM-killed), never runs `build/` and `build-clang/` together (a `vcpkg install` counts as a build),
and worktrees live in `C:\repos`, or the compiler cache silently stops sharing:
[build-trees-and-windows-asan.md §5–§7](docs/agent-rules/build-trees-and-windows-asan.md).

**Working directory matters.** `OloEditor`, `OloRuntime` and `OloServer` resolve assets, shaders and
Mono assemblies relative to `OloEditor/`; run them with `cwd = OloEditor/` (the VS Code tasks in
[.vscode/tasks.json](.vscode/tasks.json) do). The test binary runs from the repo root.

Targets: `OloEngine` (static lib), `OloEditor`, `OloRuntime`, `OloServer`, `OloEngine-Tests`,
`OloEngine-LuaScriptCore`, and (Visual Studio generator only) `OloEngine-ScriptCore` for C#.
`OloEngine` and `OloEngine-ScriptCore` depend on `GenerateBindings` (see *OloHeaderTool*).

## Tests

GoogleTest, registered in [OloEngine/tests/CMakeLists.txt](OloEngine/tests/CMakeLists.txt).

```powershell
build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=SuiteName.TestName
```

Two independent axes: the **renderer pyramid** (L1–L11 plus plumbing/cullinglod/shaderpipe/
integration/meta) and the **Functional** axis (cross-subsystem seams driven by the real
`Scene::OnUpdateRuntime`). Pick the classification before writing code:
[testing-architecture.md](docs/agent-rules/testing-architecture.md), [docs/testing.md](docs/testing.md).

Rebase modes: `--olo-golden-rebase`, `--olo-perf-rebase`, only after a deliberate visual change or a
hardware move. Benchmark captures (issue #974): `--olo-capture-manifest=<manifest>`, see
[docs/guides/renderer-benchmarks.md](docs/guides/renderer-benchmarks.md).

### Rendering changes must be visually verified

A renderer change can pass every CPU test and look broken on screen. For anything that changes what
the screen shows, do all three:

1. **Pin the math** with a CPU/contract test (runs in CI).
2. **Capture PNGs from several camera angles and look at them.** Follow
   [WaterVisualEvidenceTest.cpp](OloEngine/tests/Rendering/PropertyTests/WaterVisualEvidenceTest.cpp):
   render the real pipeline, read back, write to `OloEditor/assets/tests/visual/`, read the PNG.
   Such tests use `EnableRendering(w, h)` + `RunFrames` / `RunEditorFrames` and skip cleanly
   without a GL 4.6 context; never `DISABLED_`.
3. **Run it in the editor** and check `OloEngine.log` for shader errors. The `run-oloengine`
   skill's `attach` action starts the editor with the MCP diagnostics server; `olo_screenshot`,
   `olo_camera_*`, `olo_shader_errors` and `olo_render_capture_target` inspect the live frame.
   Guide: [docs/guides/mcp-diagnostics-server.md](docs/guides/mcp-diagnostics-server.md).

Do not report a rendering change done on unit tests alone. If you cannot inspect a frame, say so.
If you reach for an `olo_*` tool that does not exist, add a one-line bullet to the open MCP
tracker (#607) rather than working around it silently.

## Architecture

C++23, OpenGL 4.6 with DSA, Vulkan behind the RHI.

- `OloEngine/vendor/`: FetchContent downloads. Never edit; reconfigure wipes it. vcpkg ports live in
  `<buildDir>/vcpkg_installed/`, equally not yours; patch via `cmake/overlay-ports/`.
- Linux is a CI target for tests, runtime and server, not the editor
  ([ADR 0015](docs/adr/0015-editor-is-windows-only-linux-ships-runtime-only.md)).

Cross-cutting patterns:

- **ECS:** EnTT under an `Entity` wrapper. Hot loops use owning groups; a component may be owned by
  one group only, so read the ownership map at the top of `Scene/Scene.cpp` before adding one.
- **Gameplay scheduler (issue #453):** per-tick systems register with declared reads/writes and the
  order is derived. Declare the real data flow, not a position; marking a system `Parallelizable()`
  needs a thread-safety audit and an EnTT storage pre-warm. Rules:
  [notes-gameplay-physics-nav.md §11](docs/agent-rules/notes-gameplay-physics-nav.md),
  [notes-core-and-threading.md §16](docs/agent-rules/notes-core-and-threading.md).
- **Assets:** a new asset type needs loader, registration, YAML serialization and hot-reload
  handling.
- **Primitives:** `Ref<T>` from `Core/Ref.h`; `u32`, `f32`, `sizet` from `Core/Base.h`, global scope.

### OloHeaderTool

[tools/OloHeaderTool/](tools/OloHeaderTool/) generates the scripting glue, component lists, scene
serializer blocks and MCP field registry from `OloEngine/src/`, as the depfile-gated
`GenerateBindings` target. Generated files are tracked: if they look stale, build `GenerateBindings`
and re-stage them. Details: [component-serializer-codegen.md](docs/agent-rules/component-serializer-codegen.md).

### Editor undo for components

`DrawComponent<T>` records undo only for a trivially copyable or equality-comparable component. To
opt a non-trivial one in, give it `auto operator==(const T&) const -> bool = default;`
([cpp-coding-quality.md §7](docs/agent-rules/cpp-coding-quality.md)).

## Conventions

- C++23, 4-space indent, braces on new lines except trivial cases.
- Classes `PascalCase`, members `m_PascalCase`, statics `s_PascalCase`. GPU-mirror structs use bare
  `PascalCase` fields and `Pad0`-style padding; the GLSL side keeps `_padding0` on purpose
  ([cpp-coding-quality.md §13](docs/agent-rules/cpp-coding-quality.md)).
- Project headers `#include "..."`, third-party `#include <...>`; `#pragma once`; headers self-contained.
- Never `==` / `!=` on floats or glm types; validate every float from YAML/JSON/network with `std::isfinite`.
- New **engine-owned** sequence/string data is `TArray<T>` / `FString`. The binding surface (entt,
  yaml-cpp, sol2, Mono, ImGui, Jolt, spdlog, and component `std::string` fields) keeps `std::`, and
  so does every map — the `TMap` gate is closed (#1411). A relocation mistake is green on MSVC and
  aborts under libstdc++, and the trait itself answers differently on clang-cl and MSVC for a
  type holding a `std::atomic`: [engine-owned-containers.md](docs/agent-rules/engine-owned-containers.md).

## Common pitfalls

- **Wrong working directory** → missing shaders or Mono assemblies at startup.
- **A helper struct named `*Component`** → the header-tool scan sweeps it into every generated list
  and the build fails in unrelated TUs. Use another suffix.
- **Golden images as the primary check** → an L1–L5 contract test is required as well.
- **A vcpkg port at a different version than the pin** → check the registry version before moving
  a dependency ([vcpkg-dependency-management.md](docs/agent-rules/vcpkg-dependency-management.md)).
- **`[ScriptEngine] OloEngine-ScriptCore assembly unavailable` in the log** → the
  `OloEditor → OloEngine-ScriptCore` dependency edge in `CMakeLists.txt` is missing; see
  [notes-editor-and-assets.md](docs/agent-rules/notes-editor-and-assets.md).

## Writing docs here

Every PR may add a postmortem, so style and size matter more than completeness:

- **Rule first, story second.** State what to do in the first sentence; the failure that taught it
  follows. An index entry is one plain sentence saying the rule, not a teaser.
- **Say what you mean.** No metaphor where a literal phrase exists.
- **Keep a guide under about 10 KB.** Split it or push history into an appendix at the end.
- **Never add the lesson to this file.** It loads into every session. Add the file, then one line
  in the README's subsystem index and one row in its failure-mode table.

## Agent skills

- Issue tracker: GitHub Issues at `drsnuggles8/OloEngineBase` via `gh`.
- ADRs in `docs/adr/`; read them before proposing structural changes.
- Project skills: `run-oloengine`, `start-work`, `cleanup-worktree`, `resume-worktrees`.

## For other agents

`AGENTS.md` points here. Claude Code runs some rules as hooks; without them, do these yourself:
run `pre-commit run --all-files` at the end of a change and before every push, route every build
through `build-lock.ps1`, and give every issue filed from a task worktree the `Not fixed in …
because:` line and `olo-score` block from task-loop Phase 1a. The `/start-work` command is also
Claude Code specific. Everything else applies as written.
