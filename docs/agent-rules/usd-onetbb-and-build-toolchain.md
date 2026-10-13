# OpenUSD: one oneTBB, and how its build is driven (#1575)

How OpenUSD's ExternalProject gets its oneTBB, its compiler and its job count, and the check that
keeps a second oneTBB out of every link. The rest of the USD story (the cache, the plugInfo tree,
`/WHOLEARCHIVE`) is in [asset-import-usd-alembic.md](asset-import-usd-alembic.md).

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

**The job count is decided at build time, by `cmake/vendor/build-usd-config.cmake`.** It passes
`build-lock.ps1`'s memory-aware `CMAKE_BUILD_PARALLEL_LEVEL` as `--parallel`, or the physical core
count without it. A bare `--parallel` ran the native tool's own default (every core for MSBuild) and
ignored the lock; no flag at all builds MSBuild's projects one at a time.
