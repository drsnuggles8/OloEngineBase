# Never run two CMake configures against one build tree — and the error names the wrong thing

**The rule:** one `cmake` configure at a time per binary directory. Two configures on the same
build tree corrupt each other. The top-level `CMakeLists.txt` now takes a `file(LOCK ...)` on
`<binaryDir>/olo-configure.lock` for exactly this reason, so a second configure waits instead of
racing. Do not remove that lock, and do not start a configure "just to be sure" while another one
is running.

**What it looks like when it happens.** Not a message about concurrency, and not one that points at
the tool that lost:

```
CMake Error: This should not have happened. If you see this message, you are probably using a
broken CMakeLists.txt file or a problematic release of CMake
CMake Error: This should not have happened. If you see this message, you are probably using a
broken CMakeLists.txt file or a problematic release of CMake
CMake Error at .../Modules/CheckIPOSupported.cmake:197 (try_compile):
  Failed to configure test project build system.
Call Stack (most recent call first):
  cmake/CommonProperties.cmake:44 (check_ipo_supported)
  OloEngine/CMakeLists.txt:68 (olo_enable_lto)
-- Configuring incomplete, errors occurred!
```

Read literally, that says your CMakeLists.txt is broken and LTO is unsupported. Both are false. The
two bare "This should not have happened" lines are stderr from the **nested** cmake that
`try_compile()` spawns; it fails because the other configure deleted the scratch project out from
under it. Any check that shells out to a nested cmake can be the one that reports it —
`check_ipo_supported`, `CMakeTestCXXCompiler`, any `try_compile` — so the file and line in the error
tell you only who lost the race, never that there was one.

The giveaway is that the *same* command succeeded minutes earlier against the *same* tree. When a
configure failure is not reproducible, suspect a second configure before suspecting the toolchain.

## Reproducing it

Deterministic in about a minute, no engine build needed. Two configures, the second started two
seconds into the first:

```bash
mkdir -p /tmp/ipo/src && cat > /tmp/ipo/src/CMakeLists.txt <<'CM'
cmake_minimum_required(VERSION 3.25)
project(ipotest C CXX)
include(CheckIPOSupported)
foreach(i RANGE 1 25)
  check_ipo_supported(RESULT r OUTPUT o)
endforeach()
CM
cmake -S /tmp/ipo/src -B /tmp/ipo/bin -G "Ninja Multi-Config" > a.log 2>&1 &
sleep 2; cmake -S /tmp/ipo/src -B /tmp/ipo/bin -G "Ninja Multi-Config" > b.log 2>&1
```

Both logs carry the error above. Add the `file(LOCK ... GUARD PROCESS TIMEOUT 600)` from the
top-level `CMakeLists.txt` at the head of the test project and both configures pass, the second
simply taking the first one's runtime plus its own.

## Where the second configure comes from

Rarely from a person deciding to run two. The sources seen here, in order:

- **VS Code's CMake Tools extension.** It defaults to `cmake.configureOnOpen: true` and
  `cmake.configureOnEdit: true`, so opening the folder or saving any CMake file starts a configure
  with no prompt. Both are set to `false` in [.vscode/settings.json](../../.vscode/settings.json);
  configure deliberately, from the palette or the CLI.
- **A second agent session or terminal** in the same worktree. Worktrees do not help here — the
  collision is per *binary directory*, and two sessions in one worktree share `build-cached/`.
- **A retry after an apparent hang.** The first configure was still running.

The build mutex in [build-lock.ps1](../../.claude/skills/run-oloengine/build-lock.ps1) does **not**
cover this, by design: it serialises builds against memory exhaustion, and its `PreToolUse` guard
deliberately classifies `cmake` without `--build` as cheap. Cheap it is — a configure is not going
to OOM the box. Concurrency-safe it is not. The two mechanisms guard different things, and the
CMake-side lock is the one that covers configures.

## Why this repo was unusually exposed

`check_ipo_supported()` is not a cheap predicate: each call generates a `_CMakeLTOTest-<lang>`
project under the current binary directory and runs a full nested CMake configure, compile and link
on it. `olo_enable_lto()` used to call it every time, and it is called per target — `OloEngine`,
each engine part in the loop at `OloEngine/CMakeLists.txt`, and every app through
`olo_configure_app()`. That is roughly 25 nested configures per configure of this repo, each one a
window for the race and each one recomputing the same toolchain-wide answer.

`olo_check_lto_support()` in [CommonProperties.cmake](../../cmake/CommonProperties.cmake) now caches
it in `OLO_LTO_SUPPORTED` (`CACHE INTERNAL`, so it survives a re-configure of the same tree). One
nested configure instead of 25. Measured on the isolated repro above: 25 checks took 43s, one takes
about 8s.

Keep both defences. The cache narrows the window; only the lock closes it.

## The related trap next door

The same failing log carried a second, unrelated problem worth recognising: `CMAKE_TOOLCHAIN_FILE`
pointing into `Microsoft Visual Studio/18/Community/VC/vcpkg` while `VCPKG_ROOT` was `D:\vcpkg` in
every shell. The Visual Studio developer environment sets `VCPKG_ROOT` to its own bundled copy, and
CMake Tools applies that environment before expanding `$env{VCPKG_ROOT}` in the presets. The
top-level `CMakeLists.txt` now warns about it. It is not what broke the configure — a changed
`CMAKE_TOOLCHAIN_FILE` is ignored on a re-configure, so an existing tree stays pinned to whichever
vcpkg it was created with — but a *fresh* tree configured from such a window silently uses a
different registry clone that lacks `vcpkg.json`'s `builtin-baseline`. See
[vcpkg-dependency-management.md](vcpkg-dependency-management.md).
