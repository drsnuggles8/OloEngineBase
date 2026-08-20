# A configure-time guard that no-ops when its input is unset

**Postmortem of issue #828.** A fresh CMake configure on a machine with `STEAMWORKS_SDK_ROOT` set
produced an `OloEngine-Tests.exe` that would not start. The build only failed at the very end, in
gtest discovery:

```
CMake Error at .../GoogleTestAddTests.cmake:233 (message):
  Error running test executable.
    Path: .../build/OloEngine/tests/Debug/OloEngine-Tests.exe
    Result: Exit code 0xc0000135
```

`0xC0000135` is `STATUS_DLL_NOT_FOUND`. Run by hand the exe printed nothing and exited with that
code. **Nothing in the failure named Steam**, which is what made it expensive — and the known
workaround, *configure twice*, made it read as "works on my machine".

## The mechanism

Two independent facts combined.

**1. A CMake variable is only visible to a subdirectory added after it is set.** `OloEngine/CMakeLists.txt`
processed `add_subdirectory(tests)` near its top (line 49) but resolved `OLO_STEAM_RUNTIME_DLL`
near its bottom (line 335/369). So when `OloEngine/tests/CMakeLists.txt` called
`olo_copy_steam_runtime(OloEngine-Tests)`, the variable did not exist yet.

**2. The guard treated "unset" as "nothing to do."**

```cmake
if(OLO_WITH_STEAM AND NOT OLO_WITH_STEAM_STUB_SDK AND DEFINED OLO_STEAM_RUNTIME_DLL)
```

That `DEFINED` term was meant to say *"a stub build has no DLL to stage."* It also silently
absorbed *"the path has not been computed yet"* — a completely different situation, and a bug.
The copy step was simply never generated, the exe linked against `steam_api64.lib` (an **import**
library) with no DLL beside it, and the failure surfaced hours later wearing an unrelated face.

**Why only a fresh configure.** The final `set(... CACHE INTERNAL ...)` persists the path. On the
second configure the cache entry already exists when `tests/` is processed, the guard passes, and
the DLL gets staged. The bug is invisible to anyone with a warm build tree — which is everyone,
most of the time.

**Why CI never caught it.** The runners have no Steamworks SDK, so `OLO_WITH_STEAM` is OFF there
and the branch is never entered. This class of bug is only reachable on a developer machine.

## The fix

Two changes, one for the ordering and one for the silence.

**Move the resolution above every `add_subdirectory()`.** The path resolution and validation moved
out of `OloEngine/CMakeLists.txt` into `cmake/Steamworks.cmake`, included from the root
`CMakeLists.txt` alongside the other configuration modules. The **target wiring** — include
directories, the import library, the compile definitions — stayed in `OloEngine/CMakeLists.txt`,
because it needs the `OloEngine` target to exist.

That split is the durable part. A block that only computes variables has no ordering requirement of
its own, so it can be hoisted to the earliest point that makes every consumer correct. A block that
touches targets cannot. Keeping them together is what forced the resolution down to where the
target lived, and the consumers were four directories away.

Merely reordering within `OloEngine/CMakeLists.txt` would have fixed the symptom and left the trap
armed: any subdirectory added above the Steam block later would reintroduce it.

**Make the silent case loud.** `olo_copy_steam_runtime` now `message(FATAL_ERROR ...)`s when Steam
is on with the real SDK and the path is unset — naming Steam, naming `0xC0000135`, and naming the
ordering requirement. If someone re-arms this, they get a sentence at configure instead of a status
code at test discovery. It is safe in CI, which never enters that branch.

## The general rule

Ask of every `if(DEFINED X)` guard: **does the code distinguish "X does not apply here" from "X has
not been computed yet"?** If a single condition covers both, one of them is a silent failure waiting
for an ordering change. The two remedies compose:

- **Hoist pure-variable computation** above every consumer, so ordering cannot go wrong.
- **Make the impossible case fatal**, so if it does go wrong it says so where the cause is, not
  where the consequence is.

This is not specific to CMake, but CMake makes it especially easy: directory-scoped variables mean
"visible" is a property of *where* a line sits in the include/subdirectory graph, and nothing warns
you when you read one that is not there yet — an undefined variable expands to the empty string.

Related: `olo_copy_ffmpeg_runtime` in the same file carries the sibling constraint (an
`add_custom_command(TARGET ...)` must run in the directory that created the target), and
[steamworks-platform-integration.md](steamworks-platform-integration.md) covers the Steamworks
seam itself — including the *other* silent-drop in this area, an SDK path set one level too high.

## Verifying a fix in this area

A rebuild in an already-configured tree proves nothing, because the cache papers over the bug.
**Delete the build tree and configure from scratch.** The cheap check does not need a build at all:
grep the generated project for the staging step.

```powershell
cmake --preset msvc
Select-String -Path build\OloEngine\tests\OloEngine-Tests.vcxproj -Pattern steam_api64.dll
```

Zero matches on a fresh configure is the bug; matches in all four consumers (`OloEngine-Tests`,
`OloEditor`, `OloRuntime`, `OloServer`) is the fix. Check the Steam-OFF and
`OLO_WITH_STEAM_STUB_SDK=ON` configures too — a new fatal guard is exactly the kind of change that
fires where it should not.
