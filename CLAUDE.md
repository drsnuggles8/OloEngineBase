# CLAUDE.md

Rules for an agent working anywhere in this repository. Rules that apply only to some files live in
[.claude/rules/](.claude/rules/) and load when you touch a matching file; the reasons and stories
live in the linked guides. Other agents: see [AGENTS.md](AGENTS.md).

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

The `olo-command-guards` mod refuses the gated actions unless the command ends in
`# OLO_USER_APPROVED`, which you add only when the user approved that specific action.

## Scope

The request sets the scope, and the whole request is the deliverable. Three house rules on extras:

- **A bug you find on the way is fixed, not filed.** Fix it in its own commit on the same branch
  and list it in the PR body. File an issue only when the fix needs a decision from the user, needs
  access you lack, is bigger than the task itself, or touches files another live worktree owns,
  and say which in the issue. A wider diff is not a reason. Rules and issue format:
  [task-loop.md Phase 1a](docs/process/task-loop.md#1a-bugs-you-find-on-the-way--fix-them-here).
- **Tests** go where the task asks for them or where this repo already tests that kind of change,
  sized like the neighbouring test files. Scratch checks are not committed.
- **A new postmortem** is welcome, but it is a separate file with a pointer, never a paragraph added
  to this one ([.claude/rules/writing-docs.md](.claude/rules/writing-docs.md)).

## Definition of done

When a task touched code or assets:

1. **Pre-commit runs itself.** A `Stop` hook runs `pre-commit run --all-files` at the end of every
   turn. If it reformatted anything, re-stage and commit again; a commit that triggers an auto-fix
   aborting is expected. Don't run `pre-commit` by hand unless it failed. A push in the same turn as
   the edits would race the hook, so a `PreToolUse` guard runs it first and blocks the push if the
   tree changes; re-stage its fixes and push again.
2. **Every test `.cpp` is classified** with `// OLO_TEST_LAYER: <id>`
   ([.claude/rules/tests.md](.claude/rules/tests.md)).
3. **An ECS component change covers every cross-binding touch-point** no hook catches
   ([.claude/rules/ecs-components.md](.claude/rules/ecs-components.md)).
4. **A rendering change is visually verified**, not reported done on unit tests alone
   ([.claude/rules/renderer-verification.md](.claude/rules/renderer-verification.md)).

## Finding the rules

- **Path-scoped rules** in [.claude/rules/](.claude/rules/) load on their own when you read or edit
  a matching file: shaders, renderer, ECS components, tests, the gameplay scheduler, assets,
  dependencies, docs, Claude configuration.
- **Guides** in `docs/agent-rules/` hold the full rules. [Its README](docs/agent-rules/README.md)
  indexes them by subsystem, [failure-modes.md](docs/agent-rules/failure-modes.md) by symptom.
  Rather than reading either whole, ask the `rules-lookup` agent for the rules that apply to your
  files or symptom.
- The coding rules are [cpp-coding-quality.md](docs/agent-rules/cpp-coding-quality.md). Read
  [sonarqube-review-alignment.md](docs/agent-rules/sonarqube-review-alignment.md) before
  `/code-review`.

## Build & run

**`VCPKG_ROOT` must be set** (issue #773); a configure without it stops at a guard.
**`STEAMWORKS_SDK_ROOT` is optional.** Both: [.claude/rules/dependencies.md](.claude/rules/dependencies.md).

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

The lock returns the exit code of the **last** command in `-Command`: keep the build the only
command, and check the log for `build stopped` before trusting a 0.

A build **outside** the lock is never uncapped (`--parallel N` or `ninja -jN`; the box has been
OOM-killed), never runs `build/` and `build-clang/` together (a `vcpkg install` counts as a build),
and worktrees live in `C:\repos`, or the compiler cache silently stops sharing:
[build-trees-and-windows-asan.md §5–§7](docs/agent-rules/build-trees-and-windows-asan.md).

**Working directory matters.** `OloEditor`, `OloRuntime` and `OloServer` resolve assets, shaders and
Mono assemblies relative to `OloEditor/`; run them with `cwd = OloEditor/` (the VS Code tasks in
[.vscode/tasks.json](.vscode/tasks.json) do). The test binary runs from the repo root.

Targets: `OloEngine` (static lib), `OloEditor`, `OloRuntime`, `OloServer`, `OloEngine-Tests`,
`OloEngine-LuaScriptCore`, and `OloEngine-ScriptCore` / `Sandbox-Scripting` for C# (`dotnet build`
under any generator; needs the .NET SDK, `OLO_WITH_CSHARP` defaults ON on Windows).
`OloEngine` and `OloEngine-ScriptCore` depend on `GenerateBindings` (OloHeaderTool, see
[.claude/rules/ecs-components.md](.claude/rules/ecs-components.md)).

## Tests

GoogleTest, registered in [OloEngine/tests/CMakeLists.txt](OloEngine/tests/CMakeLists.txt).

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=SuiteName.TestName
```

(`build\...` for the `msvc` tree.) Where a new test belongs, and the rules for writing one:
[.claude/rules/tests.md](.claude/rules/tests.md), [docs/testing.md](docs/testing.md).

## Architecture

C++23, OpenGL 4.6 with DSA, Vulkan behind the RHI. ECS is EnTT under an `Entity` wrapper.

- `OloEngine/vendor/`: FetchContent downloads. Never edit; reconfigure wipes it. vcpkg ports live in
  `<buildDir>/vcpkg_installed/`, equally not yours; patch via `cmake/overlay-ports/`.
- Linux is a CI target for tests, runtime and server, not the editor
  ([ADR 0015](docs/adr/0015-editor-is-windows-only-linux-ships-runtime-only.md)). ADRs live in
  `docs/adr/`; read them before proposing structural changes.
- **Primitives:** `Ref<T>` from `Core/Ref.h`; `u32`, `f32`, `sizet` from `Core/Base.h`, global scope.

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
- **`[ScriptEngine] OloEngine-ScriptCore assembly unavailable` in the log** → the
  `OloEditor → OloEngine-ScriptCore` dependency edge in `CMakeLists.txt` is missing, or the tree was
  configured with `OLO_WITH_CSHARP=OFF`; see
  [notes-editor-and-assets.md](docs/agent-rules/notes-editor-and-assets.md).

## Agent workflow

- Issue tracker: GitHub Issues at `drsnuggles8/OloEngineBase` via `gh`.
- Skills (manual, `/name`): `start-work`, `cleanup-worktree`, `resume-worktrees`; `run-oloengine`
  builds, runs and screenshots the apps.
- Subagents in [.claude/agents/](.claude/agents/): `rules-lookup`, `spec-reviewer`, `ci-triage`,
  `live-verifier`. [task-loop.md](docs/process/task-loop.md) says which phase uses which.
- The `olo-*` mods hold the process rules in the tool loop; they must be installed once per machine
  ([claude-code-mods.md](docs/agent-rules/claude-code-mods.md#installing-them)).

## When compacting

Preserve: the task's issue number and `HANDOVER.md` path, the branch and PR number, every file
changed so far, the build and test commands in use, the verification-matrix rows run and still
owed, the bugs found (fixed with commit sha, or filed with issue number), and unresolved review
threads.
