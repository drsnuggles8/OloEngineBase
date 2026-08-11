# Spike results — vcpkg manifest + static-md/clang-cl triplets under Ninja Multi-Config

Issue: [#774](https://github.com/drsnuggles8/OloEngineBase/issues/774) (de-risks [#773](https://github.com/drsnuggles8/OloEngineBase/issues/773))
Machine: this dev box, 2026-08-11. vcpkg tool `2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`, vcpkg registry checked out at that same clone's `HEAD` (shallow clone, no explicit `builtin-baseline` pin — see Residual risks).

## TL;DR — GO

All 5 "what to prove" items pass. Two real gotchas were found and fixed (both are one-line, well-understood, documented below). The binary-cache cross-worktree sharing — the make-or-break item — works cleanly and is dramatically faster than a cold build, as expected.

## The 5 items

| # | Item | Result |
|---|---|---|
| 1 | Manifest + both triplets configure and build under both generators (VS 18 2026 `build/`-style, Ninja Multi-Config `build-clang/`-style) | **PASS** — see `custom-triplets/`, `CMakeLists.txt`. Both generators configure, build, and the resulting `spike_consumer.exe` runs successfully. |
| 2 | The three deps link into a throwaway consumer TU (glm, spdlog logging, Jolt body construction), proving the `static-md` CRT choice | **PASS** — `main.cpp` builds and runs under both generators, dynamic-CRT/static-lib linkage confirmed (no CRT-mismatch heap corruption, no link errors). |
| 3 | The jolt overlay port builds with `CROSS_PLATFORM_DETERMINISTIC=ON` and the `rtti` feature | **PASS** — confirmed directly in Jolt's own `CMakeCache.txt` for *both* triplets (`CROSS_PLATFORM_DETERMINISTIC:BOOL=ON`, `CPP_RTTI_ENABLED:BOOL=ON`), and functionally via a `dynamic_cast<const JPH::BoxShape*>` in the consumer, which requires RTTI to succeed. |
| 4 | Binary cache shares across worktrees — build once, a second worktree restores prebuilt archives instead of recompiling | **PASS**, and more rigorously than planned: rather than a second build directory in the *same* vcpkg install, a **second, independently bootstrapped vcpkg installation** (fresh clone, fresh `bootstrap-vcpkg.bat`, its own empty `downloads/`/`packages/`/`buildtrees/`) was pointed at the same `VCPKG_DEFAULT_BINARY_CACHE`. It restored all 6 packages (both triplets) with **zero source downloads and zero recompilation** in 484–639 ms. |
| 5 | Measure cold dependency-build wall-clock: FetchContent-today vs vcpkg-cold vs vcpkg-cache-hit | **Measured** — see table below. |

## Timing table (3 deps: glm, spdlog[fmt], joltphysics[rtti]; MSVC unless noted; `--parallel 6` throughout)

| Scenario | Time | Notes |
|---|---|---|
| **FetchContent, today's model** (clone + CMake configure) | 35.6 s | Non-shallow clones of all 3 repos, matching `OloEngine/vendor/CMakeLists.txt`'s `GIT_SHALLOW FALSE` |
| **FetchContent, today's model** (build) | 22.6 s | Single unified build graph, all 3 deps + consumer compiled in one parallel pass |
| **FetchContent total (cold, per worktree, every time)** | **58.2 s** | This is paid in full by *every* worktree, *every* time, forever |
| **vcpkg cold install, MSVC triplet** (`x64-windows-static-md`) | **2 m 7 s** | First-ever build on this machine; empty binary cache. Includes glm, spdlog, joltphysics, fmt (transitive), + 2 vcpkg-cmake helper ports |
| **vcpkg cold install, clang-cl triplet** (`x64-windows-static-md-clangcl`) | **51 s** | Same machine, source downloads already warm from the MSVC pass above (not a from-empty-download-cache number — see caveat below) |
| **vcpkg cache-hit, MSVC triplet, independent second vcpkg install** | **639 ms** (relevant packages restored) / 458 ms total install action | Simulates a second worktree/machine sharing only the archive cache — the realistic worst case |
| **vcpkg cache-hit, clang-cl triplet, independent second vcpkg install** | **484 ms** restore / 1.4 s total install action | Same simulation, clang-cl triplet |

**Caveats on these numbers:**
- This is a 3-dependency sample, not the full 25-dependency migration — see Residual risks for why the cold-build comparison may not extrapolate linearly.
- The clang-cl cold number (51 s) reused already-downloaded source tarballs from the MSVC pass; a genuinely from-scratch clang-cl-only cold build would include download time too (glm/spdlog/jolt/fmt source, roughly what the FetchContent clone step pays: ~35 s). So the fairest "worst case, nothing warm at all" clang-cl cold number is closer to **~85 s**.
- vcpkg installs ports **strictly sequentially** ("Installing 1/6… 2/6…"), each internally parallelized across `--parallel`/`VCPKG_MAX_CONCURRENCY` cores. FetchContent instead builds one unified graph where all 3 deps' translation units interleave across every core simultaneously. For 3 deps this made FetchContent's cold build *faster* than vcpkg's cold build (58 s vs 127 s) — the opposite of the naive expectation. This is a genuine, not-obvious cost characteristic; see Residual risks.

### What the timing actually buys (the point of #773)

The win is not "vcpkg cold is faster" (it measured slower here, for 3 deps) — it's that **every worktree after the first pays the cache-hit cost, not the cold-build cost.** Illustrating with these 3-dep numbers, MSVC triplet:

| Worktrees opened | FetchContent-today (linear) | vcpkg (1 cold + N-1 cache-hit) |
|---|---|---|
| 1 | 58 s | 127 s |
| 3 | 175 s | ~128 s |
| 5 | 291 s | ~130 s |
| 10 | 582 s | ~133 s |

FetchContent crosses over and loses by worktree 2–3 even on this tiny 3-dep sample; the real 25-dependency vendor tree (CLAUDE.md: ~0.9 GB pre-build) will cross over far sooner in absolute terms. **This number should be re-measured at full 25-dependency scale before the #773 migration is scored as fully de-risked** — see Residual risks.

## Gotchas found (both fixed, both one-line)

### 1. `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` + a toolchain that resolves the compiler by bare name fails immediately

Our production `cmake/ClangCLToolchain.cmake` sets `CMAKE_C_COMPILER "clang-cl"` (a bare name, resolved via `PATH` — fine for the engine's own interactive CMake configure). Under vcpkg, every port failed at `enable_language()`:

```
CMake Error at CMakeLists.txt:11 (enable_language):
  The CMAKE_C_COMPILER: clang-cl
  is not a full path and was not found in the PATH.
```

vcpkg builds each port in a curated environment and does **not** pass the invoking shell's `PATH` through by default. Fix: `set(VCPKG_ENV_PASSTHROUGH PATH)` in the triplet.

### 2. Plain `VCPKG_ENV_PASSTHROUGH` bakes the variable's *value* into the ABI hash — defeats cross-worktree cache hits

After fixing #1, a second configure with a differently-ordered (but functionally identical) `PATH` string produced a **different port ABI hash** and triggered a full rebuild instead of a cache hit — silently defeating the entire point of #773. `PATH` routinely differs byte-for-byte across worktrees/shells/CI runners even when it resolves to the same `clang-cl.exe`. Fix: `set(VCPKG_ENV_PASSTHROUGH_UNTRACKED PATH)` instead — makes the compiler resolvable without hashing the value, so the cache key stays stable across machines/shells with equivalent-but-differently-ordered `PATH`.

**This is the more dangerous of the two** — #1 fails loudly on the very first build. #2 fails silently: builds still succeed, they're just slower than they should be, and nothing in the log calls it out as wrong unless you're specifically watching for cache-restore vs. rebuild.

### 3. (Minor, not vcpkg-specific) `glm/gtx/string_cast.hpp` needs `GLM_ENABLE_EXPERIMENTAL`

Unrelated to vcpkg — glm's own experimental-header gate. Listed here only because it was the first build failure encountered and is worth knowing before the real migration's consumer code touches this header.

## Overlay port note

The registry `joltphysics` port ([`ports/joltphysics/portfile.cmake`](https://github.com/microsoft/vcpkg/blob/master/ports/joltphysics/portfile.cmake) as vendored into the local vcpkg clone) hardcodes `-DCROSS_PLATFORM_DETERMINISTIC=OFF` as a plain CMake option, exactly as the #773 audit predicted — not a feature, so it can't be selected from the manifest. The overlay in `overlay-ports/joltphysics/` is a 1-line diff (`OFF` → `ON`) plus a version bump note. `rtti` is already a registry feature and needed no override. Confirmed via `CMakeCache.txt` inspection (see item 3 above), not just "the option was passed" — the value actually landed.

## Residual risks (things #773 should still verify, not blockers)

1. **Cold-build timing at full 25-dependency scale is unmeasured.** vcpkg's strictly-sequential per-port install (vs. FetchContent's single parallel build graph) means the cold-build cost comparison found here (vcpkg cold *slower* than FetchContent cold, for 3 deps) may or may not hold at 25 deps — it depends on how much of the real vendor tree's build time is dominated by a few large deps (assimp, protobuf, MaterialX) vs. spread evenly. The cache-hit win is unambiguous regardless; the cold-build win is not proven either way at scale.
2. **No `builtin-baseline` was pinned in `vcpkg.json`** — this spike relies on whatever happened to be at the local vcpkg clone's `HEAD` (a shallow, single-commit clone). The real migration needs a pinned baseline SHA for the same reproducibility reasons the Jolt commit pin exists (see `CLAUDE.md`'s `#281` note) — an unpinned baseline means two developers running `vcpkg install` on different days could silently get different port versions.
3. **GameNetworkingSockets/OpenSSL and the other 3 "needs a decision" deps from the #773 audit were not touched by this spike** — it exercised the mechanism classes (manifest, static-md triplet, clang-cl chainload, feature selection, overlay port, cache sharing), not every specific dependency's specific quirk.
4. **`vcpkg-cmake`/`vcpkg-cmake-config` host-triplet packages were built as `x64-windows` (dynamic), not the custom triplet** — this is vcpkg's own standard behavior for host tools and is expected, not a finding, but worth being aware of: these two are the only things in the dependency graph that *don't* go through our custom triplet.

## Reproduction

```powershell
# One-time, machine-global (see D:\vcpkg in this session's environment — adjust path per machine):
git clone https://github.com/microsoft/vcpkg D:\vcpkg
D:\vcpkg\bootstrap-vcpkg.bat

$env:VCPKG_ROOT = "D:\vcpkg"
$env:VCPKG_DEFAULT_BINARY_CACHE = "D:\vcpkg-binary-cache"
$env:VCPKG_MAX_CONCURRENCY = "6"

# MSVC generator (mirrors build/):
cmake -S spike -B spike/build-vs -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DVCPKG_OVERLAY_TRIPLETS=spike/custom-triplets `
  -DVCPKG_OVERLAY_PORTS=spike/overlay-ports
cmake --build spike/build-vs --config Release --parallel 6
spike/build-vs/Release/spike_consumer.exe

# Ninja Multi-Config + clang-cl generator (mirrors build-clang/, the `clangcl` preset):
cmake -S spike -B spike/build-ninja-mc -G "Ninja Multi-Config" `
  -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md-clangcl `
  -DVCPKG_OVERLAY_TRIPLETS=spike/custom-triplets `
  -DVCPKG_OVERLAY_PORTS=spike/overlay-ports `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
cmake --build spike/build-ninja-mc --config Release --parallel 6
spike/build-ninja-mc/Release/spike_consumer.exe
```

Build directories and `vcpkg_installed-*` are not committed (see the repo root `.gitignore`'s `/spike/...` entries) — they're multi-hundred-MB and fully reproducible from the commands above plus a warm `D:\vcpkg-binary-cache`.
