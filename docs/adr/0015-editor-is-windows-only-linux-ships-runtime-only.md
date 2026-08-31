# Linux ships as a native runtime, not under Proton — and the editor stays Windows-only

Issue [#892](https://github.com/drsnuggles8/OloEngineBase/issues/892), the second of *Drift*'s
(#878) four shipping gates, asks two questions in a deliberate order: **native Linux vs Proton**
first, because the answer resizes everything else, and then whether the *editor* needs to exist on
Linux at all. This ADR answers both, backed by what CI already proves and by an actual Linux build
attempt of `OloEditor` (see "Build attempt" below).

## Question 1 — native Linux, or Windows-under-Proton for the shipped game?

**Native.** This is not a green-field choice: `OloRuntime` and `OloServer` are already
continuously built and tested on Linux, and have been for a while.

- `.github/workflows/vulkan-off.yml` builds `OloEngine-Tests`, `OloRuntime` **and** `OloServer`
  on `ubuntu-24.04` — on every PR that touches the relevant paths, on every push to `master`, and
  on a weekly schedule. `video-ffmpeg.yml` and `steam-stub.yml` also build and run on
  `ubuntu-24.04`, but only `OloEngine-Tests` — they exercise the engine core on Linux, not
  `OloRuntime`/`OloServer` specifically.
- `.github/workflows/gpu-conformance-amd.yml` runs the **full test suite, goldens and visual
  evidence included**, against a real AMD Navi 10 (RX 5600 XT) via Mesa radeonsi on a self-hosted
  Linux runner — genuine cross-vendor hardware validation, not a software rasterizer stand-in.

That is a stronger position than "we could probably get Proton to work." Proton adds a
translation layer (DXVK/VKD3D, FEX where relevant, Wine's Win32 surface) between the game and the
Linux kernel/driver for a payoff that only matters if we *didn't* already have a native Linux
runtime — we do, and it is hardware-validated on every relevant CI run. Shipping under Proton would
mean deliberately routing around a path we already exercise natively, to reach a compatibility
layer we have never tested against. There is no scenario here where Proton is the better bet.

Concretely: Drift ships a native Linux `OloRuntime`/`OloServer` build. Steam Deck compatibility
follows from that (native Linux + Proton *not required* is the best-case Deck compatibility
rating), and the sibling packaging issue (#894, steamcmd/depot automation) targets the native Linux
depot, not a Proton-tagged Windows one.

## Question 2 — does the editor need to ship, or even build, on Linux?

**No.** Drift is authored on Windows; only the runtime ships to and runs on Linux. This is the
"most likely" outcome the issue itself predicted, and the build attempt below confirms there is no
forcing reason to change it.

### Build attempt

Before ruling the editor out, we tried to actually build it, per the issue's explicit instruction
not to guess. Environment: WSL2 Ubuntu 24.04, clang-19 (the compiler every Linux CI job in this
repo already pins for the C++23 standard library), the `x64-linux` vcpkg triplet, Ninja, on
`origin/master` @ `bb68d50b8` — the same commit this worktree branched from.

**Result: it does not build clean, but it gets close, and both blockers found are real,
specific, and understood — not a wall of unknown breakage.**

1. **Configure succeeds outright.** CMake resolves the full dependency graph — OpenVDB, Vulkan
   (`glslc`/`glslangValidator` found), OpenGL, Boost — with zero platform-specific configure
   errors. `OloEditor`, like every other target, is not gated behind a Windows-only
   `if(WIN32)` in the root `CMakeLists.txt`.
2. **736 of 798 build steps succeed**, including the entire `OloEngine` static-library chain
   (core, renderer, content) and every vendored dependency (imgui, imguizmo, glad, lua,
   xatlas, bc7enc). The Windows-guarded `#ifdef OLO_PLATFORM_WINDOWS` blocks around
   `<Windows.h>` in `ContentBrowserItem.cpp`/`ContentBrowserPanel.cpp` — the two editor files
   that looked riskiest on an unguarded grep — compile out cleanly, as intended.
3. **First real blocker: `OloRuntime` (not `OloEditor` specifically) fails to *link*** with
   `ld.lld: error: undefined symbol: std::__stacktrace_impl::_S_current(...)` from
   `Platform/OpenGL/OpenGLDebug.cpp`'s `std::stacktrace::current()` call. `OloEngine/CMakeLists.txt`
   already has machinery for exactly this (`OLO_HAVE_LIBSTDCXXEXP`, a `check_linker_flag(CXX
   "-lstdc++exp" ...)` with a GCC-only glob fallback) — but in this exact environment (WSL2
   Ubuntu 24.04, clang-19 selecting its GCC-14 install) the `check_linker_flag` call itself
   evaluates false, even though `clang++-19 -lstdc++exp` links a trivial program with no error
   when tried directly. The existing fallback is gated `elseif(CMAKE_CXX_COMPILER_ID STREQUAL
   "GNU")`, so it never engages for Clang. This did not reproduce as a documentation error: the
   repo's Linux CI jobs (`vulkan-off.yml` et al.) build `OloRuntime` with the identical
   clang-19/`ubuntu-24.04` pairing and are green, so the check evidently passes there — this
   looks like an environment-specific `try_compile` quirk rather than a universal break, and
   root-causing it further is out of this issue's scope. Passing `-lstdc++exp` explicitly via
   `CMAKE_EXE_LINKER_FLAGS` unblocks the link.
4. **With that workaround, the build proceeds all the way into `OloEditor`'s own sources** and
   hits two genuine, editor-specific compile errors in
   `OloEditor/src/Panels/SceneHierarchyPanel.cpp` (lines 4261 and 4590):

   ```
   error: use 'template' keyword to treat 'GetComponent' as a dependent template name
       connectedLabel = opt->GetComponent<TagComponent>().Tag;
   ```

   This is a standard two-phase-name-lookup strictness difference: MSVC accepts
   `opt->GetComponent<TagComponent>()` on a dependent type without disambiguation, Clang (and
   GCC) require `opt->template GetComponent<TagComponent>()`. A real, narrow, two-line
   portability bug — not evidence of a deep architectural problem, and not touched here per the
   "prove, not necessarily fix" scope of this issue.
5. **`OloEngine-ScriptCore` never entered the picture** — its `CMakeLists.txt` inclusion is
   itself gated `if(CMAKE_GENERATOR MATCHES "Visual Studio")`, so under Ninja (the only
   generator this attempt could use on Linux) C# scripting is not merely broken, it is absent
   from the build graph entirely. Per `CLAUDE.md`'s existing note, a Linux editor build would
   ship with no working C# scripting even past the two errors above.

Net: with two known, fixable snags — one link-flag detection gap and one two-line dependent-name
fix — `OloEditor` is plausibly reachable on Linux. It is not there today, and per the decision
below, nobody is going to finish that walk, because there is no one who needs to.

### Why "no" even setting the build result aside

1. **A game can be authored on one platform and shipped on another.** Nothing about Drift's asset
   pipeline, scene format, or build tooling requires the machine that ships the binary to be the
   machine that edited the scene — `OloRuntime` already loads exactly what `OloEditor` produces,
   cross-platform, because scenes/assets are data, not compiled editor state.
2. **The team's actual editing machines are Windows.** There is no workflow today, or asked for by
   any issue, that has a developer opening `OloEditor` on Linux. Building it there would be
   maintaining a capability nobody uses.
3. **`OloEngine-ScriptCore` (C#/Mono) only builds under the Visual Studio generator** — a Linux
   editor would ship without working C# scripting, or would need a second scripting-build path
   built and maintained for a platform with no actual editing users. That is real, ongoing cost for
   a capability with no consumer.
4. **The cost of being wrong is low and reversible.** If a Linux editor is ever genuinely needed —
   a contributor who only has Linux, say — the runtime's Linux CI coverage means the hard part
   (engine core, RHI, asset loading) is already proven there; only the editor-specific surface
   (ImGui platform backend, native file dialogs — already implemented per PR #365's
   zenity/kdialog work, panels) would be new ground.

## Decision

- **Ship Drift as a native Linux runtime build.** Not Proton.
- **`OloEditor` is Windows-only**, by decision, not by oversight. It is not built, tested, or
  supported on Linux, and no CI job builds it there.
- The sibling packaging issue (#894) targets a native Linux depot for `OloRuntime`/`OloServer`; it
  does not need a Proton-compatibility layer or a Linux editor artifact.

## What this ADR does NOT do

- It does not add a CI job that builds `OloEditor` on Linux. Per the decision above, there is
  nothing for such a job to protect — it would be spending CI minutes keeping a capability green
  that nobody uses, exactly the failure mode `docs/agent-rules/build-trees-and-windows-asan.md` and
  the vulkan-off.yml header warn against ("a reassuring no-op"). If the editor is ever put in
  scope, add the job at that point, modeled on `vulkan-off.yml`'s Linux job shape.
- It does not touch `OloEditor/src` to fix or work around anything the build attempt found. The
  brief was "prove or drop," not "make it work" — see the acceptance criteria in #892.

## Consequences

- `CLAUDE.md`'s `OloServer/` line, which understated Linux coverage as "the only target that runs
  on WSL2," is corrected to describe what CI actually builds and to point here.
- A future contributor asking "why doesn't the editor build on Linux" has a recorded answer instead
  of an unexamined gap — the exact outcome #892 was filed to force.
- If this decision is ever revisited, it is a genuine reconsideration (new information: a Linux
  contributor, a marketing reason to demo the editor cross-platform), not a rediscovery of facts
  already known today.

## Status

Accepted.
