# A green Windows build does not prove a header is self-contained

**Rule: a header that uses a name must include the header that declares it, even when the build is
green.** On Windows the precompiled header hides most omissions, so "it compiles here" is not
evidence. The Linux jobs are the only place the class shows up, and they show it to whoever adds
the *next* include, not to whoever wrote the omission.

## Why Windows cannot see it

`OloEnginePCH.h` carries `Core/Base.h`, `Debug/Instrumentor.h`, `<windows.h>` and much else, and
every engine `.cpp` includes it as its first line. Any header reached from such a TU therefore
compiles with that whole set already in scope, whether or not it asked for it. Three consequences:

- **The failure is latent, not absent.** A header missing `Debug/Instrumentor.h` compiles for years.
  It becomes a hard error the first time someone includes it from a TU that lacks the macro — a
  test, a tool, or a Linux job.
- **The blame lands on the wrong change.** On PR #1062 a one-line `#include "Renderer3D.h"` in
  `VulkanDrawPathTest.cpp` produced `Renderer3D.h:1620: error: use of undeclared identifier
  'OLO_PROFILE_FUNCTION'` on both Linux jobs. The defect was in `Renderer3D.h` and predated the PR.
- **Turning the PCH off does not fix it either.** The `dev-cached` preset disables PCH (a
  PCH-consuming compile is uncacheable), but every engine `.cpp` still `#include`s `OloEnginePCH.h`
  explicitly as line 1. Same masking, different mechanism.

## How to prove it instead

Compile the header as its own translation unit, with the target's real flags and **no** PCH:

```bash
# one-line TU per header, then:
clang-cl -fsyntax-only <the target's flags from build-cached/compile_commands.json> tu.cpp
```

`compile_commands.json` from a `dev-cached` configure already has PCH-free flags, so lifting a
representative entry, stripping `/Fo`, `/Fd` and `-c`, and appending `-fsyntax-only` is the whole
setup. It takes seconds per header and is the only local check that answers the question the Linux
jobs ask. Configuring the tree is the expensive part (~4 min); the sweep itself is not.

Measured on #1071 across the 31 headers that use `OLO_PROFILE_*`: 18 failed before the sweep with
198 `OLO_PROFILE` error lines, 0 after.

## The ratchet

`scripts/check_header_profiling_include.py` (pre-commit hook `header-profiling-include`) is a
textual guard for this one macro family: a header under `OloEngine/src` that expands a macro
`Instrumentor.h` defines must include `Instrumentor.h` (or `Debug/Profiler.h`, which includes it),
or opt out with `// OLO_PROFILING_INCLUDE_OK: <reason>`. It reads the macro set out of
`Instrumentor.h` so it cannot drift. It is deliberately narrow — it is a ratchet on the class that
already bit us, not a self-containment check.

**Check where the include is, not just that it is there.** An include below the first expansion is
not in scope at that expansion; the header still fails to compile alone, and a presence-only grep
calls it clean. The exception is a use inside a `#define` body — that expands at the call site in
some other file, so it obliges this header to own the include but says nothing about its position
(`Core/PerformanceProfiler.h` is the case in the tree).

**Keep the configuration flags in the checked set, not just the call macros.** `OLO_PROFILE` and
`OLO_FUNC_SIG` are the silent half of this bug: an undefined name in `#if` evaluates to 0 with no
diagnostic, so a header testing `#if OLO_PROFILE` without the include does not fail to compile — it
changes shape between translation units. `Task/InheritedContext.h` gated `FInheritedContextScope`'s
members and its move constructor's initializer list on `#if OLO_PROFILE && TRACY_ENABLE`, which is
an ODR mismatch that links clean. Widening the checked set from the two call macros the issue named
to every macro `Instrumentor.h` defines found that one and `Core/PerformanceProfiler.h`.

## Two things the textual grep and the compile check disagree about, and why both are right

- **Four of the 21 headers the grep flagged already compiled standalone** (`CircularBuffer.h`,
  `MemberList.h`, `NetworkPriorityQueue.h`, `Oversubscription.h`) — they reached the macro through a
  sibling include. Still a violation: the sibling is free to drop that include tomorrow.
- **The grep missed nothing here but is not a superset.** A grep for
  `OLO_PROFILE_FUNCTION\|OLO_PROFILE_SCOPE` also matches `#define OLO_PROFILE_RENDERER_SCOPE` in
  `Renderer/Debug/RendererProfiler.h`, which defines an unrelated macro family and is not a defect.
  Match on the names the defining header actually defines, and drop the `#define <NAME>` prefix
  before looking for call sites.

## Still open: `<windows.h>` macro leakage

Three headers still fail a standalone compile on Windows for a *different* reason, and #1071 did not
touch them: `Task/Pipe.h`, `Task/ParallelFor.h` and `Scene/Entity.h`. `Task/TaskShared.h` includes
`<tracy/Tracy.hpp>` directly, which pulls `<windows.h>` in with `min`, `max` and `Yield` as macros;
`HAL/PlatformProcess.h` then declares a static member called `Yield()` and the parse fails
(`expected member name or ';' after declaration specifiers`). Defining `NOMINMAX` clears the first
two, nothing clears `Yield`. It is invisible in a normal build because `OloEnginePCH.h` defines
`NOMINMAX` before anything, and invisible on Linux because there is no `<windows.h>`. Route
`<windows.h>` through `Platform/Windows/MinWindows.h` (which already sets the guards) rather than
letting a vendor header drag it in.
