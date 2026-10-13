# OpenUSD: its oneTBB, its build and its link

How OpenUSD's ExternalProject gets its oneTBB, its compiler and its job count, how its per-machine
cache is shared, the check that keeps a second oneTBB out of every link, and why the engine
force-links `usd_m` by full path. The importer, the plugInfo tree and the recipe are in
[asset-import-usd-alembic.md](asset-import-usd-alembic.md).

## 1. One oneTBB: vcpkg's

**USD compiles against vcpkg's oneTBB and the engine links vcpkg's `TBB::tbb`; nothing else may
bring a oneTBB library.** The `usd` feature in `vcpkg.json` installs `tbb`. `cmake/vendor/OpenUSD.cmake`
finds it `GLOBAL` (OpenVDB's lookup then reuses the same target) and hands its `TBB_DIR` to the USD
build. The USD cache key names the full oneTBB version (`usd-<sha8>-tbb<x.y.z>`), because `usd_m`
compiles oneTBB's headers into its objects.

USD used to build its own oneTBB 2022.2 beside vcpkg's 2023.1, so every USD-enabled executable linked
two static oneTBB runtimes. USD's archive came first on the link line, so OpenVDB, compiled against
2023.1 headers, ran on 2022.2's internals, and `/FORCE:MULTIPLE` would have hidden any clash.
`-DOLO_USD_INSTALL_DIR` is refused at configure: a prebuilt install carries its own oneTBB.

## 2. The guard

`cmake/OneTbbGuard.cmake` fails the build, before `OloEngine` compiles, if any executable or shared
library links more than one oneTBB core library (`tbb12[_debug].lib` on Windows, `libtbb[_debug]`
`.a`/`.so`/`.dylib` elsewhere; `tbbmalloc` is not counted). It reads the link line CMake generated,
per configuration, from the File API codemodel, so a raw path, a generator expression or another
directory's imported target cannot slip past it.

- **Ask with `cmake_file_api(QUERY ...)`, not a query file.** CMake reads query files before it
  configures, so a query file written during configure is answered only by the next run. A fresh
  tree's first build then had no codemodel; a configure-dependency marker made Ninja regenerate,
  but the Visual Studio generator's ZERO_CHECK ignored it.
- **Negative control:** link USD's old `tbb12.lib` beside vcpkg's and build `olo_onetbb_guard`. It
  must fail, naming every binary and both archives.

## 3. USD v25.11 against oneTBB 2023.1

**It does not build unpatched.** Its `workTBB` headers test `TBB_INTERFACE_VERSION_MAJOR >= 12`
without including `<tbb/version.h>`. 2022.2's `blocked_range.h` brought it in; 2023.1's does not, so
the macro reads as 0 and USD compiles its legacy `tbb::task` path
(`error C2516: 'tbb::v1::task': is not a valid base class`). `cmake/vendor/patch-usd-tbb-version.cmake`
adds the include. A oneTBB bump changes the cache key; check the patch still applies.

USD's objects carry `/DEFAULTLIB:"tbb12.lib"` (`tbb12_debug.lib` in Debug) from oneTBB's headers.
Both linkers satisfy it from the full-path `TBB::tbb` already on the link line, so no link directory
is needed: a `link.exe` probe links with the full path and fails `LNK1104` without it.

**The Debug teardown assert.** A static Debug oneTBB can trip a benign worker-teardown assert
(`small_object_pool.cpp:143`, `m_private_counter >= 0`) at exit. USD's own oneTBB build compiled it
out with `-DTBB_USE_ASSERT=0`; vcpkg's `tbb12_debug.lib` keeps it. The Debug test binary exits 0
after a USD stage import and the OpenVDB imports; the Debug editor's close was not measured. If the
assert returns, set the define for the `tbb` port in an overlay port, not in the triplet, which every
port's ABI hash reads.

## 4. The compiler and the job count

**On Windows USD is built by MSVC's `cl`, through a Visual Studio generator.** An ExternalProject
inherits the generator, not the compilers. Under a Ninja tree with no MSVC environment its configure
found LLVM's GNU-style `clang++` on `PATH`, and USD stopped at `'cxxabi.h' file not found`
(`arch/demangle.cpp`). The parent's `clang-cl` fails too: USD's defaults treat any Clang as
GNU-style and pass `-Wno-deprecated`, which clang-cl reads as `/wd`. So a non-VS tree builds USD
with `OLO_USD_VS_GENERATOR` (the msvc preset's generator by default), which locates the toolset
itself. The bug was latent until #1575 changed the cache key: every earlier cache had been seeded by
a Visual Studio tree, and the Ninja trees only ever hit it.

**The job count is decided at build time, by `cmake/vendor/build-usd-config.cmake`:**
`build-lock.ps1`'s memory-aware `CMAKE_BUILD_PARALLEL_LEVEL`, or the physical core count without it.
Under a Visual Studio generator it goes to `/MP`, not MSBuild's `/m`: USD compiles every project with
`/MP`, so `/m:N` would run N projects each starting a compiler per core. The script builds one
project at a time with `CL_MPCount=N` and `/nodeReuse:false` (reused MSBuild nodes hold handles in
the shared cache after the build). A bare `--parallel` ignored the lock; no flag built MSBuild's
projects one at a time.

## 5. The cache is shared across worktrees; the stamps are not

The cache holds the source/binary/install trees, but `ExternalProject`'s `STAMP_DIR` defaults to the
**consuming** build tree. So a **second worktree** used to see no stamps, conclude every step was out
of date, and re-run configure+build *inside the first worktree's* `usd-build` — which is not
idempotent, because `BUILD_COMMAND` is `--config Release` **then** `--config Debug` in one binary dir.
Re-entering a finished tree fails across USD's `arch` target with:

```text
error C2859: ...\arch.dir\Release\arch.pdb is not the pdb file that was used when this
             precompiled header was created, recreate the precompiled header.
```

The artifacts were shared; the "already done" record was not. **Symptom to recognise:** a *fresh
worktree* failing in `olo_openusd` while `install/{Debug,Release}` is demonstrably complete
(`usd_m.lib` ≈ 2.4 GB Debug / 2.1 GB Release).

`OpenUSD.cmake` now **short-circuits on a complete cache**: if `usd_m` + the config's oneTBB lib +
`include/pxr` + the `lib/usd` plugInfo tree all exist for **both** configs, it skips
`ExternalProject_Add` entirely (`olo_usd_ext` becomes a no-op) and consumes the cache as a prebuilt
install — so no worktree after the first ever writes to the shared tree. A *partial* cache (an
interrupted first build) fails the check and resumes the real build. Bumping `_OLO_USD_TAG` changes
the cache key; to force a genuine rebuild, delete `$LOCALAPPDATA/OloEngine/usd-<sha8>`.

Two things that follow:

- **Moving `STAMP_DIR` into the cache would NOT have fixed this** — it only relocates the race to two
  worktrees configuring for the first time simultaneously.
- **Still unsafe: two worktrees running their FIRST USD build concurrently** (one shared binary dir).
  Same rule as the msvc/clangcl trees in
  [build-trees-and-windows-asan.md](build-trees-and-windows-asan.md) — sequence the first build; every
  tree after it is a lock-free cache hit.

A key change leaves the previous `usd-*` directory behind (about 10 GB); delete it by hand. Changing
`OLO_USD_VS_GENERATOR` on a partial cache needs its `usd-build` deleted too, or the nested configure
stops on a generator mismatch.

## 6. `/WHOLEARCHIVE:` needs a FULL PATH

A bare library name is not portable across the two Windows linkers (#697). `link.exe` resolves a bare `/WHOLEARCHIVE:usd_m` against `/LIBPATH`;
**`lld-link` does not** — it opens the argument as a path relative to the working directory only,
and kills the whole link with

```text
lld-link: error: could not open 'usd_m': no such file or directory
```

even though `usd_m.lib` is already on the link line by full path and its directory is in
`/LIBPATH`. This is not a niche path: **CMake sets `MSVC` TRUE for clang-cl** (it targets the MSVC
ABI), so an `if(MSVC)` guard is taken under the `clangcl` / `clangcl-asan` presets too. The result
was that both of those presets could not link *at all* with the default `OLO_WITH_USD=ON`, while
the msvc preset was perfectly green — everything compiled, so it looked like a link-config problem
rather than an option-spelling one.

The fix is to spell the option with the full path CMake already has
(`target_link_options(OloEngine PUBLIC "/WHOLEARCHIVE:${OloEngine_USD_LIB}")`), which is accepted
by **both** linkers. Verified against **LLD 21.1.8** and **MSVC `link.exe` 14.51** with a minimal
force-link probe (a static lib whose only content is a static-init side effect, and a `main` that
never references it):

| `/WHOLEARCHIVE:` argument                   | `link.exe` | `lld-link` |
| ------------------------------------------ | ---------- | ---------- |
| bare name, lib reachable via `/LIBPATH`     | force-links | **link fails** |
| full path, lib also on the link line        | force-links | force-links |
| full path, lib *not* on the link line       | force-links | force-links |
| full path containing spaces                 | force-links | force-links |
| *(control)* no `/WHOLEARCHIVE` at all       | object dropped | object dropped |

The control row is the part worth keeping: it is what proves the probe measures force-linking
rather than incidental reachability.

Do **not** "fix" a `/WHOLEARCHIVE` failure by deleting the option. It is load-bearing under both
linkers, but in *different* ways — measured by relinking the engine with the option commented out:
under `link.exe` the link succeeds and USD fails at **runtime** (gotcha 4), while under `lld-link`
the link fails outright with
`undefined symbol: __declspec(dllimport) ... UsdGeomGetStageUpAxis` — because `usd_m`'s headers
declare its symbols `dllimport` (the `/IGNORE:4217` flood) and lld-link will not satisfy a
dllimport-declared symbol from the archive without the whole-archive pull. So "it linked after I
removed the flag" is a *link.exe-only* observation, and it is the silent-at-link,
broken-at-runtime case.

`OloEngine_USD_LIB` carries a `$<CONFIG>` genexpr and `target_link_options` expands
it per config, so the full-path spelling is also config-matched where the bare name depended on
`/LIBPATH` search order to land on the right one.
