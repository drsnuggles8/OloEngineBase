# vcpkg dependency management

How third-party dependencies are resolved since issue #773, what the migration
actually bought, and the traps that cost time — several of which fail **silently**.

Read this before adding, removing, or version-bumping a dependency, and before
touching `vcpkg.json`, `cmake/triplets/`, or `cmake/overlay-ports/`.

## The model in one paragraph

`vcpkg.json` at the repo root declares the dependencies. The CMake presets point
`CMAKE_TOOLCHAIN_FILE` at `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`,
which installs the manifest into `<binaryDir>/vcpkg_installed/` during the first
`project()` call; `OloEngine/vendor/CMakeLists.txt` then resolves them with
`find_package()`. Ports are built once per (port, feature set, triplet,
toolchain) into a machine-global, ABI-hash-keyed **binary cache**, so the first
worktree to build a given combination populates it and every other worktree
restores prebuilt archives in seconds. A curated list of dependencies is still
fetched in-tree; `OloEngine/vendor/CMakeLists.txt`'s header comment names each
one and why.

## Setup (one time per machine)

```powershell
git clone https://github.com/microsoft/vcpkg D:\vcpkg
D:\vcpkg\bootstrap-vcpkg.bat

setx VCPKG_ROOT D:\vcpkg                                # persists for FUTURE shells only
$env:VCPKG_ROOT = "D:\vcpkg"                            # ...so set it here too, or reopen the shell
setx VCPKG_DEFAULT_BINARY_CACHE D:\vcpkg-binary-cache   # optional; defaults under %LOCALAPPDATA%
```

`VCPKG_ROOT` is the only required variable — every preset reads it. A configure
without it fails at the guard in the root `CMakeLists.txt` with a pointer here,
rather than as a wall of "could not find a package configuration file".

**Do not clone vcpkg shallow if you might ever need a version override.**
A single-commit clone can resolve the pinned `builtin-baseline` (its git-trees
are all reachable from that one commit) but *cannot* resolve an `"overrides"`
entry that holds a port at an older version — the older port's git-tree object
simply is not in the object database. `git fetch --unshallow` if you hit that.

## Trap 1 — `core.fsmonitor` makes vcpkg fail with a file-lock error

Symptom, on every `vcpkg install` in this repo, at whichever port happens to be
checked out first:

```text
error: rename_or_delete("…/buildtrees/versioning_/versions/assimp/<tree>_35780.tmp",
                        "…/buildtrees/versioning_/versions/assimp/<tree>"):
  The process cannot access the file because it is being used by another process.
note: while checking out port assimp with git tree <tree>
```

vcpkg checks each port version out into a `.tmp` directory and then **renames**
it into place. `git config core.fsmonitor true` makes git spawn a
`git fsmonitor--daemon` that watches the worktree and holds open handles inside
it — and on Windows an open handle makes that rename fail. It looks like a vcpkg
bug or antivirus; it is neither, and it reproduces every run.

**Fix:** `git -C $VCPKG_ROOT config core.fsmonitor false`. Nothing about a
read-only registry clone benefits from fsmonitor. `.github/actions/setup-vcpkg`
does this for CI. Note the daemons are *per repository*, so this can also be
triggered by an unrelated repo on the same box: a FetchContent configure that
clones 35 dependencies leaves 35 daemons running.

## Trap 2 — the CRT triplet is `static-md`, not `static`

`cmake/triplets/x64-windows-static-md.cmake` is static libraries against the
**dynamic** CRT. The commonly-cited `x64-windows-static` uses the *static* CRT
and is wrong here: three project settings (`USE_STATIC_MSVC_RUNTIME_LIBRARY
OFF`, `protobuf_MSVC_STATIC_RUNTIME OFF`, `gtest_force_shared_crt ON`) plus
`cmake/CommonProperties.cmake`'s `MSVC_RUNTIME_LIBRARY` all establish the
dynamic CRT.

A CRT mismatch between a vcpkg-built static lib and the engine is **heap
corruption across a library boundary at runtime, not a link error.** Nothing in
the build output flags it.

## Trap 3 — build ports ONCE, with cl.exe, for both Windows trees

The `clangcl` preset compiles **our** code with clang-cl and links with lld-link, but its
`VCPKG_TARGET_TRIPLET` is the plain `x64-windows-static-md` — the same ports the `msvc`
tree uses. That is deliberate and evidence-backed; do not "fix" it by adding a clang-cl
triplet back.

The #774 spike built a clang-cl-chainloaded triplet
(`x64-windows-static-md-clangcl`) and proved the mechanism works. In the full migration it
turned out to cost more than it was worth:

- **It broke MaterialX.** With ports built by clang-cl,
  `MeshInterchangeTest.MaterialXReadsStandardSurfaceFactors` fails — `.mtlx` parsing
  silently returns constructor defaults instead of the authored values. Same engine code,
  same lld-link, ports rebuilt with cl.exe: the test passes, and the full suite comes back
  **5688 / 6 / 2, byte-for-byte identical to the msvc tree**. The fault is in third-party
  code compiled by clang-cl, not in our code or the linker.
- **It broke libsodium**, whose non-MSVC autotools path fails under a chainloaded clang-cl
  at `configure: error: C compiler cannot create executables` (vcpkg-make hands libtool's
  `compile` wrapper a mangled command line). That needed a per-port triplet exception.
- **It doubled the cost**: a second full set of ~45 packages, a second ~8 min cold install,
  a second 2.75 GB install tree, and zero shared binary-cache entries between the trees.
- **It bought nothing measurable.** The clangcl preset exists to catch Clang warnings in
  *our* code. Third-party code was always `-Wno-error`'d out of that, and post-#773 it is
  not compiled by us at all.

Mixing is safe by construction: clang-cl targets the MSVC ABI on purpose, which is the
entire reason it exists. The whole-suite run above is the empirical confirmation.

### The gotcha to remember if a chainloaded triplet ever comes back

`VCPKG_ENV_PASSTHROUGH` folds the passed-through variable's **value** into each port's ABI
hash. A chainloaded toolchain that names its compiler by bare name (as
`cmake/ClangCLToolchain.cmake` does) needs `PATH` passed through to resolve it at all —
but `PATH` differs byte-for-byte across worktrees, shells and CI runners even when it
resolves to the same `clang-cl.exe`. With the plain form, every such difference is a cache
**miss**: builds still succeed, they are just orders of magnitude slower than a restore,
and nothing in the log says so. Use `VCPKG_ENV_PASSTHROUGH_UNTRACKED` — it makes the
variable visible without hashing its value.

## Trap 4 — a project-local configuration silently links Debug third-party libs

This project has three configurations — `Debug`, `Release`, **`Dist`** — but
every vcpkg package installs exactly two (Debug + Release). For an IMPORTED
target with no matching configuration, CMake falls back to the first entry of
`IMPORTED_CONFIGURATIONS`, in practice **Debug**. A `Dist` build would then link
debug third-party libraries against release engine code: an
`_ITERATOR_DEBUG_LEVEL` / CRT mismatch, i.e. a runtime heap fault, with a green
build log.

`cmake/SetupConfigurations.cmake` sets `CMAKE_MAP_IMPORTED_CONFIG_DIST
"Release;"` before the first consuming target is created. **Any new project-local
configuration needs the same line.**

## A dependency's option names are part of its contract — verify them, don't assume

Moving Jolt to a port surfaced two things that had been wrong or invisible for a long
time. Both are the same mistake in different directions: **a CMake `set()` for an option
name the dependency never reads is silently a no-op.**

**1. Cross-platform determinism was never actually on.** The in-tree build did

```cmake
set(JPH_CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)   # <- wrong name
```

but Jolt's `Build/CMakeLists.txt` declares `option(CROSS_PLATFORM_DETERMINISTIC ... OFF)`
— **no `JPH_` prefix**. `JPH_CROSS_PLATFORM_DETERMINISTIC` is the *compile definition* Jolt
emits when that option is on, not the option itself. So the cache variable was set, nothing
read it, the option stayed OFF, and a comment two lines above asserted determinism was
enabled. The overlay port passes `-DCROSS_PLATFORM_DETERMINISTIC=ON`, which means this
migration turned determinism on **for the first time** — a real change in physics
behaviour, and the thing issue #281 wanted. (`JPH_BUILD_SHARED_LIBRARY` and
`JPH_STATIC_LIBRARY` in the same block are also not Jolt option names; they were harmless
only because Jolt defaults to static.)

Contrast `CPP_RTTI_ENABLED`, set right next to them — that one *is* a real Jolt option, so
RTTI genuinely was on. Same file, same block, one name right and one wrong, no signal
either way.

**2. Two features silently switched off.** Jolt defaults
`DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` and `PROFILER_IN_DEBUG_AND_RELEASE` to **ON**, and the
in-tree build never overrode them — so the engine had `JPH_DEBUG_RENDERER` and
`JPH_PROFILE_ENABLED`. In vcpkg those are opt-in *features*, so a manifest requesting only
`rtti` turns both off. The engine adapts silently: `Physics3DSystem` and
`JoltLayerInterface` guard on `#if defined(JPH_PROFILE_ENABLED)`, and `JoltCaptureManager`'s
capture path needs `JPH_DEBUG_RENDERER`. Nothing fails to build; the code just stops
existing. The manifest now requests `debugrenderer` and `profiler` to restore parity.

**Generalisable:** when moving a dependency to a port, diff what its *defaults* gave you
against what the manifest's feature list asks for. Inherited-by-default behaviour is
invisible in the old build files precisely because nobody had to write it down. And check
each `set()` against the dependency's own `option()` declarations — a typo'd or prefixed
name never warns.

(`Jolt::Jolt` does export `JPH_CROSS_PLATFORM_DETERMINISTIC` in its
`INTERFACE_COMPILE_DEFINITIONS`, so engine TUs compiling Jolt inline code agree with the
library. Jolt's determinism build also wants precise floating point; MSVC's default is
already `/fp:precise` and this project sets no `/fp:` flag, so they match.)

## Trap 5 — "the overlay was found" is not "the option took effect"

vcpkg logs `info: installing overlay port from here` as soon as it *locates* the
port directory. That says nothing about whether the option you added actually
reached the dependency's build. Verify against the port's own CMake cache:

```powershell
Select-String CROSS_PLATFORM_DETERMINISTIC `
  $env:VCPKG_ROOT\buildtrees\joltphysics\config-*-CMakeCache.txt.log
```

This matters most for Jolt: a `CROSS_PLATFORM_DETERMINISTIC` mismatch throws no
build or link error, just divergent physics — the #281 failure class.

## Trap 6 — installing a port shadows a `find_package` you did not intend to move

`find_package(Vulkan)` in `OloEngine/CMakeLists.txt` exists for two reasons: the
`Vulkan::glslang` / `Vulkan::SPIRV-Tools` imported targets, and `Vulkan_INCLUDE_DIRS`
as the path to the **shader toolchain's** headers (`spirv_cross/`, `shaderc/`,
`glslang/`), which ship inside the installed Vulkan SDK.

The moment `vulkan-headers` became a vcpkg dependency, the vcpkg toolchain started
redirecting `find_package(Vulkan)` at the port, and `Vulkan_INCLUDE_DIR` flipped from
`C:/VulkanSDK/<ver>/Include` to `<buildDir>/vcpkg_installed/<triplet>/include`. For
`<vulkan/*.h>` that is desirable — it is what makes the pinned 1.4.357 headers win
without any ordering games. But `spirv_cross/` is not in the vcpkg tree, so the build
died with 71 × `Cannot open include file: 'spirv_cross/spirv_cross.hpp'`.

**The general shape:** vcpkg's `find_package` shim redirects by package *name*, and a
package name can cover more than the thing you asked for. When you add a port, check
what else was reading the variables that `find_package` sets — not just the target you
wanted. The fix here is `OLO_VULKAN_SDK_INCLUDE_DIR`, resolved straight from
`$ENV{VULKAN_SDK}` and listed *after* the pinned headers so it cannot win the
`<vulkan/vulkan_core.h>` lookup.

This one fails loudly, at least. Its silent cousin would be a port that shadows a
`find_package` whose result still *works* but differs in version.

## Trap 7 — editing a triplet invalidates every package built with it

The triplet file's *contents* feed each port's ABI hash — that is what makes the cache
key stable across worktrees (trap 3). The flip side: a one-line edit to
`cmake/triplets/<name>.cmake` changes the hash of **every** package for that triplet
(43 on the MSVC one), so the next install rebuilds the lot. Budget for that when tuning a triplet,
and batch triplet edits rather than iterating one flag at a time.

## Per-port exceptions live in the triplet, not in a forked portfile

A triplet is loaded once *per port*, with `PORT` set, so it can carry a targeted exception
without forking upstream's portfile:

```cmake
if(PORT STREQUAL "somelib")
    set(VCPKG_BUILD_TYPE release)   # or unset a chainload, change linkage, …
endif()
```

Prefer this to an overlay port when the fix is "build this one port differently" rather
than "this port's options are wrong" — an overlay means copying the portfile, its patches
and its config templates, and re-diffing all of them on every baseline bump. Nothing needs
it today (the one user, libsodium, went away with the clang-cl triplet), but it is the
right first reach.

## The two overlay ports we own

Both live in `cmake/overlay-ports/`. Keep each a minimal diff against
`$VCPKG_ROOT/ports/<name>/`, and re-diff after every baseline bump.

| Port | Delta | Why |
|---|---|---|
| `joltphysics` | `CROSS_PLATFORM_DETERMINISTIC=ON` (one line) | The registry portfile hardcodes it `OFF` as a plain option, not a feature, so it cannot be selected from a manifest. `rtti` *is* a feature and needs no override. |
| `gamenetworkingsockets` | `USE_CRYPTO=libsodium`, `ice` off by default, explicit `sodium_*` cache seeding | The registry portfile hardcodes `USE_CRYPTO=OpenSSL` and takes an `openssl` dependency. Taking a whole TLS stack into the supply chain to avoid owning a portfile is a bad trade. GNS's bundled `Findsodium.cmake` searches a hand-rolled vendor-SDK layout that a vcpkg install does not have, *and* declares both the debug and release library paths as `REQUIRED_VARS` — so it is fed explicit cache variables instead of being allowed to search. |

## What did NOT move, and why

The #773 audit called 25 dependencies "clean moves". Six of them were not, and
the reasons are worth knowing before someone retries one:

| Dependency | Registry has | We use | Verdict |
|---|---|---|---|
| **sol2** | 3.5.0 + a Lua-5.5 patch | newer `develop` pin | Header-only ⇒ no compile to cache, so ~zero benefit; and the `SOL_ALL_INTEGER_VALUES_FIT` u64 contract (issue #643) is load-bearing and hard-won. |
| **lua** | 5.5.1 | 5.4.7 | A major-ish jump against a sol2 pin tested on 5.4. Pinning 5.4.7 needs a version override, which needs non-shallow registry history. |
| **stb** | 2024-07-29 | ~2 years newer | `stb_image` is an image **decoder fed untrusted files**. Silently reverting two years of fixes there is not a build-system decision. (vcpkg installs its own stb anyway as an assimp dependency, but flat as `<stb_image.h>`; we include `<stb_image/stb_image.h>`, so they do not collide.) |
| **imguizmo** | 1.10 | ~equivalent | The port takes a hard dependency on the **imgui port**. imgui stays in-tree (hand-picked backends + `IMGUI_IMPL_OPENGL_LOADER_GLAD=1`), so moving imguizmo would link two Dear ImGui copies into one binary. |

### What moved on a second pass, and the version cost of each

The first pass kept entt, glm and tracy in-tree because the registry is behind our
pins. Revisited with an explicit decision that **recent-but-older is acceptable** in
exchange for coverage, all three moved:

| Dep | Was | Now | What that costs |
|---|---|---|---|
| **entt** | v4.0.0 (released 2026-07-22) | **3.16.0** (2025-11-19) | A major version. Safe here only because the API surface this engine touches is tiny and v3-compatible — `entt::entity`, `type_hash`, `get`, `null`, `registry`, `id_type`, `exclude`. Verified by compiling, not by reading. Also needed the include spelling `<entt.hpp>` → `<entt/entt.hpp>`, since the port installs namespaced. |
| **glm** | master @2026-04-07 | **1.0.3** (2025-12-31) | Nothing measurable: 1.0.3 is upstream's newest *release*; we were tracking unreleased master. A full suite run on each gave identical results. |
| **tracy** | v0.14.0 (released 2026-08-09) | **0.13.1** (2025-12-11) | Two real things, below. |

**Tracy is the one with a behavioural consequence.** The port exports
`INTERFACE_COMPILE_DEFINITIONS "TRACY_ENABLE;TRACY_ON_DEMAND"`, so linking
`Tracy::TracyClient` defines both in **every** configuration. Previously the engine
defined them in Release only, and the in-tree `TracyClient` was given a matching
per-config `target_compile_definitions`. Consequences:

- Debug builds go from `E1/OD0` (profiler collecting from process start) to `E1/OD1`
  (on-demand). Less intrusive, but different.
- The client and consumer now agree *by construction*, because both sides read the
  same imported target — which matters, because **0.13.1 does not carry v0.14.0's
  config-mangled `GetProfiler` symbol**. On 0.14.0 a TRACY_ON_DEMAND mismatch was a
  loud `LNK2001`; on 0.13.1 it would be silent. We no longer have that backstop, so
  do not hand-add per-config Tracy defines on top of the imported target.

`TRACY_ENABLE` remains the CMake option gating whether Tracy is linked at all (the
TSan preset turns it off, per `cmake/Sanitizers.cmake`).

**Generalisable rule:** a dependency's *build cost* is what a binary cache buys
you. For a header-only library the win is one avoided clone, which is real but
small — so a header-only library is never worth a version regression. Check the
registry's version against the current pin *before* assuming a port is a clean
move; "a port exists" is not the same as "the port has our version".

### Attributing a golden-image failure to a dependency swap

Two visual-evidence tests failed after the migration, and the instinct — "429 glm
headers changed, glm is on the math path, glm did it" — was wrong. Worth recording,
because the reasoning generalises:

- `EASUVisualEvidenceTest.GTAOSurvivesRuntimeUpscaleSwitch` failed **in the full
  suite and passed in isolation** → order-dependent GPU/renderer state, not a
  dependency change. A failure that does not reproduce standalone is not evidence
  about your diff.
- `AtmosphereVisualEvidenceTest`'s two NIGHT captures failed **deterministically**
  (RMSE 13.08 / 9.40 vs a threshold of 8). Deterministic looks damning. But
  reverting glm to the in-tree pin and rebuilding reproduced **bit-identical** RMSE
  values — `13.077706847047567` both times. Identical to the last digit means the
  CPU-side math is byte-for-byte the same, which exonerates every header-only
  dependency at once.

**The cheap decisive test is a full-precision comparison, not a pass/fail one.** If
a metric comes back identical to 17 significant figures across a dependency swap,
that dependency is not in the causal path — no further bisection needed. If it
comes back merely *close*, it is.

(The atmosphere drift is therefore not attributable to this migration; the test's own
comments already flag its night goldens as the fragile ones. Confirming it is
pre-existing needs a master build, which was not done here.)

FFmpeg and OpenUSD also stay put — FFmpeg because it already has its own install
cache and the port is one of the registry's longest builds, OpenUSD because its
port opens with `vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)` and we need the
static-monolithic `usd_m` + `/WHOLEARCHIVE`.

## The assimp/MaterialX pugixml merge is a live hazard — check it when you touch either

`MaterialXFormat` statically compiles its own **pugixml 1.9**; assimp pulls in **pugixml
1.16** (a bundled copy before #773, the vcpkg `pugixml` port after it — the port unbundles
it and takes a real dependency). Both export the same ~794 global `pugi::` symbols:

```
llvm-nm --defined-only pugixml.lib         -> 794 pugi:: symbols
llvm-nm --defined-only MaterialXFormat.lib -> the same 794, incl.
                                              ?_create@xml_document@pugi@@AEAAXXZ
```

MaterialX's own CMake tries to contain this with `CXX_VISIBILITY_PRESET hidden`, which is
an ELF concept and a **no-op on the MSVC ABI**. So `OloEngine` links both under
`/FORCE:MULTIPLE`, which keeps the first definition, and
`MeshInterchangeTest.MaterialXReadsStandardSurfaceFactors` is the only thing standing
between that merge and silently wrong materials.

Two things were established while chasing a MaterialX failure here, both worth keeping:

- **Link order is not the lever.** Putting MaterialX ahead of assimp changes which copy
  wins and does not change the outcome either way.
- **The compiler is.** The failure tracked exactly one variable — whether the ports were
  built by clang-cl or cl.exe — which is why the fix was Trap 3's shared MSVC triplet, not
  anything to do with pugixml.

**Generalisable:** a `/FORCE:MULTIPLE` duplicate-symbol merge is a standing bet on which
two specific copies collide. Moving either participant to a registry port re-rolls it, and
the port may unbundle something the merge depended on. Before moving a dependency that
participates in a forced symbol merge, check what its port unbundles — and keep the test
that proves the merge is safe.

## Cost characteristics — measure, don't assume

vcpkg installs ports **strictly sequentially** (each port's own compile is
parallelized across cores, but independent ports do not overlap). A FetchContent
build instead compiles one unified graph where every dependency's translation
units interleave across all cores at once.

The #774 spike measured this on a 3-dependency sample and found vcpkg's **cold**
install *slower* than the equivalent FetchContent cold build (127 s vs 58 s) —
the opposite of the naive expectation. The full-scale numbers for this repo are
in the table below. **The win vcpkg provides is in the cache-hit path, not the
cold-build path.** Do not assume otherwise at any scale; measure.

### Measured on this repo (issue #773, 2026-08-11)

16-core / 64 GB Windows dev box, MSVC `x64-windows-static-md`, `OLO_WITH_USD/ALEMBIC/
MATERIALX/VULKAN=ON`, `OLO_VIDEO_FFMPEG=ON`. Dependency provisioning only — the
engine's own compile is unchanged by this migration and is excluded.

| Scenario | Time |
|---|---|
| **FetchContent, first worktree** — configure (git-clone ~35 repos + CMake configure) | **6 min 26 s** |
| FetchContent, *every subsequent* worktree — identical, no sharing | 6 min 26 s |
| **vcpkg, first worktree** — cold install, 43 packages, empty binary cache | **19 min 52 s** |
| **vcpkg, every subsequent worktree** — restore 43 packages from the binary cache | **27 s** (36 s wall) |
| vcpkg configure, cache warm (manifest restore + full CMake configure) | 57 s |

The cold-install number is dominated by five ports: protobuf 7.1 min, alembic 2.7 min,
assimp 2.3 min, materialx 1.2 min, abseil 1.2 min — together 71% of the total. The
remaining 38 packages are ~6 min combined.

The cross-worktree restore was verified the strict way: a second manifest root at an
unrelated filesystem path, `--only-binarycaching` (so a miss is a hard error, not a
silent rebuild) and a read-only `files` binary source. All 43 packages restored, zero
rebuilds.

**Disk**, same configuration — this is the one axis where vcpkg is *worse*, and it is
worth knowing before assuming the migration relieves disk pressure:

| Path | Measured |
|---|---|
| `build/vcpkg_installed/` | 4.03 GB |
| `build-clang/vcpkg_installed/` (same triplet, so a pure cache restore — but its own copy on disk) | ~4 GB |
| `$VCPKG_DEFAULT_BINARY_CACHE` (machine-global, shared by every tree and worktree) | 1.10 GB |

So ~8 GB of installed trees **per worktree**. `vcpkg_installed` is per-`binaryDir`, so the
two Windows trees each materialise a copy even though (since Trap 3) they now install the
*identical* package set. Pointing both at one `VCPKG_INSTALLED_DIR` would halve that; it is
not wired up because two configures writing one installed tree is a race, and the project
already forbids building the two trees concurrently for other reasons.

Against that, `OloEngine/vendor/` no longer carries sources or build output for the 21
migrated dependencies. `$VCPKG_ROOT/{downloads,buildtrees,packages}` is shared across every
worktree; prune `buildtrees` freely, it is intermediate object files regenerable from the cache.

If the binding constraint on the dev drive is space rather than time, this migration does
not help and may hurt.

## Forcing a cold build vs. a cache hit when debugging

```powershell
# Force a genuine from-scratch port build (bypass the binary cache):
$env:VCPKG_BINARY_SOURCES = "clear"

# Cache-hit only — fail instead of building, to PROVE a restore path works:
$env:VCPKG_BINARY_SOURCES = "clear;files,$env:VCPKG_DEFAULT_BINARY_CACHE,read"

# Which happened? The install log distinguishes them explicitly:
#   "Restored N package(s) from …"   <- cache hit
#   "Building <port>…"               <- cold build
```

`vcpkg install --dry-run` prints the full resolved plan (versions, features,
which overlay served each port) in seconds and is the right first check after
editing the manifest — it validates the baseline, the port names, the feature
names and both overlays without building anything.

## CI

`.github/actions/setup-vcpkg` is the single place CI wires this up. It resolves
`VCPKG_ROOT` (the GitHub-hosted runners' preinstalled clone, or a fresh clone
under `RUNNER_TEMP` for the self-hosted GPU runner), disables `core.fsmonitor`
(trap 1), fetches the manifest's baseline commit, and selects vcpkg's `x-gha`
binary source.

`x-gha` stores **one Actions-cache entry per package** (a few dozen), not one per
object file. That distinction matters here: this repo previously had sccache's
GitHub Actions backend flood its 10 GB cache cap with 20k+ tiny entries until
every write failed (see the `SCCACHE_DIR` note in `Windows.yml`). Per-package
granularity stays well inside the cap.
