# vcpkg manifest mode under Ninja Multi-Config + a clang-cl chainloaded triplet

Findings from the #774 spike (de-risking #773's full vcpkg migration). Read this
before wiring vcpkg into the real build — both gotchas below are one-line fixes,
but the second one fails *silently* and will quietly defeat the entire point of
the migration (cross-worktree binary-cache sharing) if missed.

## The setup

A custom triplet that chainloads our own clang-cl toolchain
(`cmake/ClangCLToolchain.cmake`) via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`, so vcpkg
builds every port with the exact compiler the `clangcl` preset (Ninja
Multi-Config) uses for the engine itself.

## Gotcha 1 — a chainloaded toolchain that resolves its compiler by bare name fails immediately under vcpkg

`ClangCLToolchain.cmake` sets `CMAKE_C_COMPILER "clang-cl"` — a bare name,
resolved via `PATH`. That's fine for the engine's own interactive CMake
configure (where `PATH` is intact), but vcpkg builds each port in a curated,
sanitized environment and does **not** pass the invoking shell's `PATH` through
to port builds by default. Every port failed identically:

```
CMake Error at CMakeLists.txt:11 (enable_language):
  The CMAKE_C_COMPILER: clang-cl
  is not a full path and was not found in the PATH.
```

**Fix:** any custom triplet that chainloads a toolchain resolving its compiler
by name (rather than an absolute path) needs:

```cmake
set(VCPKG_ENV_PASSTHROUGH PATH)
```

## Gotcha 2 (the dangerous one) — plain `VCPKG_ENV_PASSTHROUGH` bakes the variable's *value* into the ABI hash

After fixing gotcha 1, re-running the identical build with a differently
*ordered* — but functionally identical — `PATH` string produced a **different
port ABI hash**, which triggered a full rebuild instead of restoring from the
binary cache. `PATH` routinely differs byte-for-byte across
worktrees/shells/CI runners even when it resolves to the same `clang-cl.exe`
(different prepend order, an extra entry, drive-letter casing) — so a plain
passthrough silently defeats the entire point of #773: cross-worktree cache
sharing.

**This fails quietly.** Gotcha 1 breaks the very first build, loudly, with a
CMake error. Gotcha 2 doesn't break anything — builds keep succeeding, they're
just several orders of magnitude slower than they should be (full rebuild vs.
a sub-second binary-cache restore), and nothing in the log flags it as wrong
unless you're specifically diffing "Restored N package(s) from cache" against
"Installing N/M... Building...".

**Fix:** use the untracked form instead — it makes the variable visible to the
build without folding its value into the cache key:

```cmake
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED PATH)
```

## Other findings from the spike

- The registry `joltphysics` port hardcodes `-DCROSS_PLATFORM_DETERMINISTIC=OFF`
  as a plain CMake option (not a feature) — confirmed exactly as the #773 audit
  predicted. An overlay port flipping that one line to `ON` is required; `rtti`
  is already a registry feature and needs no override. Verify the flag actually
  landed by grepping the port's own `CMakeCache.txt` under
  `<vcpkg-root>/buildtrees/<port>/config-<triplet>-*-CMakeCache.txt.log` —
  don't just trust that the overlay portfile was picked up (vcpkg logs
  `info: loaded overlay triplet from here` / `info: installing overlay port
  from here`, but that only proves the file was *found*, not that the option
  it passes actually took effect in the dependency's own build).
- vcpkg installs ports **strictly sequentially** even under
  `VCPKG_MAX_CONCURRENCY`/`--parallel` (each port's own compile is
  parallelized across cores, but independent ports do not overlap). A
  FetchContent build instead compiles one unified graph where every
  dependency's translation units interleave across all cores at once. For a
  3-dependency sample this made a **cold** vcpkg install measurably *slower*
  than the equivalent FetchContent cold build (127 s vs 58 s) — the opposite
  of the naive expectation. The win vcpkg provides is entirely in the
  **cache-hit** path (a second, independently-bootstrapped vcpkg install
  restored the same 3 packages in under a second), not in the cold-build path.
  Don't assume vcpkg's cold-build time will beat FetchContent's at any scale —
  measure it, especially as dependency count grows.
- No `builtin-baseline` was needed for `vcpkg.json` to configure and build
  (vcpkg falls back to whatever's checked out in the local vcpkg clone) — but
  the real migration should pin one for the same reproducibility reason the
  Jolt commit is pinned (see this file's `CLAUDE.md` `#281` note): without a
  pin, two machines running `vcpkg install` on different days can silently
  resolve different port versions.

## Full writeup

Reproduction commands, the complete timing table, and the go/no-go rationale
are in `spike/RESULTS.md` on the `feature/vcpkg-ninja-mc-spike-774` branch
(issue #774) — that branch is throwaway and does not merge to `master`, so
this file is the durable copy of what a future spike or the real #773
migration needs to not rediscover.
