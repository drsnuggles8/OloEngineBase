# OloEngine Build Guide

## Table of Contents
- [Prerequisites (All Platforms)](#prerequisites-all-platforms)
- [Windows](#windows)
- [Linux](#linux)
- [Build Targets](#build-targets)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites (All Platforms)

| Tool        | Minimum Version | Notes                                  |
|-------------|-----------------|----------------------------------------|
| CMake       | 3.25 (raw) / **4.2+ for `CMakePresets.json`** | Root `CMakeLists.txt` requires 3.25. The shipped presets (`msvc`, `clangcl`, `clangcl-asan`) use the `Visual Studio 18 2026` generator and require CMake 4.2+. Plain `cmake -B build -G "Visual Studio 17 2022"` (no preset) still works at 3.25. |
| Git         | 2.x             | vcpkg registry + FetchContent clones   |
| **vcpkg**   | bootstrapped    | **`VCPKG_ROOT` env var must be set** — see below |
| Vulkan SDK  | 1.3+            | `VULKAN_SDK` env var must be set       |
| C++ compiler| C++23 support   | Known-working: MSVC 17.x / GCC 14+ / Clang 17+. CMake enforces `CMAKE_CXX_STANDARD = 23` (required) but does not enforce specific compiler versions; older compilers with full C++23 support may work but are untested. |
| Steamworks SDK | 1.65+ | **Optional, and not in this repo — you download it yourself.** Only needed to work on Steam features (#644); everything builds and tests without it. `STEAMWORKS_SDK_ROOT` env var. [See below](#steamworks-sdk-optional--you-must-obtain-it-yourself). |

The Vulkan SDK must include `glslc` and `glslangValidator`.

### vcpkg (one time per machine)

Since issue #773 most third-party dependencies come from the [`vcpkg.json`](../../vcpkg.json)
manifest at the repo root. Every CMake preset points `CMAKE_TOOLCHAIN_FILE` at
`$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`; a configure without `VCPKG_ROOT`
stops immediately with a pointer to this section.

```powershell
# Windows
git clone https://github.com/microsoft/vcpkg D:\vcpkg
D:\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT D:\vcpkg                                # persists for FUTURE shells only
$env:VCPKG_ROOT = "D:\vcpkg"                            # ...so set it here too, or reopen the shell
setx VCPKG_DEFAULT_BINARY_CACHE D:\vcpkg-binary-cache   # optional, but see below
git -C D:\vcpkg config core.fsmonitor false             # REQUIRED, see below
```

```bash
# Linux
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg          # exports into THIS shell; add to your profile to persist
git -C ~/vcpkg config core.fsmonitor false
```

Two things that are not optional in practice:

- **`core.fsmonitor false` on the vcpkg clone.** If git's fsmonitor daemon is enabled
  there, every `vcpkg install` fails with
  `rename_or_delete(… .tmp, …): The process cannot access the file because it is being
  used by another process` — the daemon holds handles inside the port-version checkout
  that vcpkg renames into place. It looks like antivirus; it is not, and it reproduces
  every run.
- **Do not clone vcpkg shallow.** A single-commit clone resolves the pinned
  `builtin-baseline` fine but cannot resolve a `vcpkg.json` `"overrides"` entry that
  holds one port at an older version — the older port's git-tree object is not present.

The binary cache is what makes a second worktree cheap: the first build of a given
(port, features, triplet, toolchain) populates it, and every later worktree restores
prebuilt archives instead of recompiling. It defaults to `%LOCALAPPDATA%\vcpkg\archives`
(Windows) / `~/.cache/vcpkg/archives` (Linux); setting `VCPKG_DEFAULT_BINARY_CACHE` just
moves it somewhere you control.

Background, traps and the per-dependency decisions:
[docs/agent-rules/vcpkg-dependency-management.md](../agent-rules/vcpkg-dependency-management.md).

### Steamworks SDK (optional — you must obtain it yourself)

**Nothing here is required to build, run or develop OloEngine.** Steam support is OFF by default
and the whole engine builds, runs and passes its tests without it. Skip this section entirely
unless you are working on achievements, Steam Cloud, rich presence or the Steam overlay (#644).

**The SDK is not in this repository and never can be.** It downloads from
`partner.steamgames.com` behind a Valve login, its licence forbids redistribution, and this repo
is public — so it is neither vendored in-tree nor available as a vcpkg port. Each developer
installs it themselves and points an environment variable at it, exactly like the Vulkan SDK.

It is **free**. Two different things are often confused:

| | Cost | What it gives you | Needed here? |
|---|---|---|---|
| **Steamworks SDK Access Agreement** | Free | The SDK download and the partner docs. Signed with an ordinary Steam account. | **Yes, this one** |
| **Steam Direct** | $100 per app, recoupable after $1,000 revenue | The right to publish a game on Steam | No |

Steps:

1. Sign the [Steamworks SDK Access Agreement](https://partner.steamgames.com/documentation/sdk_access_agreement)
   with your normal Steam account and download the SDK zip (`steamworks_sdk_<version>.zip`).
2. Extract it somewhere **outside this repository**, in a version-stamped directory so a future
   SDK sits alongside rather than silently replacing this one.
3. Set `STEAMWORKS_SDK_ROOT` to the **inner `sdk` directory** — the one *directly* containing
   `public/` and `redistributable_bin/`.

```powershell
# Windows — assuming the zip was extracted to D:\SDKs\steamworks_sdk_165
setx STEAMWORKS_SDK_ROOT D:\SDKs\steamworks_sdk_165\sdk        # persists for FUTURE shells only
$env:STEAMWORKS_SDK_ROOT = "D:\SDKs\steamworks_sdk_165\sdk"    # ...so set it here too
```

**Pointing the variable one level too high is the mistake everyone makes**, and it produces a
build with Steam silently switched off rather than an error. CMake detects that specific case and
tells you the corrected path:

```
CMake Warning:
  STEAMWORKS_SDK_ROOT is set one level too high, so Steam support is OFF.
    current: D:\SDKs\steamworks_sdk_165
    correct: D:\SDKs\steamworks_sdk_165\sdk
```

With the variable set, `OLO_WITH_STEAM` **auto-detects to ON** on the next fresh configure. CMake
caches option defaults, so if you install the SDK *after* configuring, pass `-DOLO_WITH_STEAM=ON`
once (or wipe the build directory) for it to take effect.

To develop against Steam without owning a published app, put `480` — Valve's public *Spacewar*
test app, which every Steam account owns — in `OloEditor/steam_appid.txt`, and have the Steam
client running. That file is git-ignored. **Remove it for a release build**: shipped games take
their App ID from the Steam client, and leaving it behind is how a build reports itself as
Spacewar.

Two things worth knowing before you go hunting for them:

- `steam_api64.lib` is an **import library** for a DLL, not a static lib, so it carries no CRT of
  its own and the `x64-windows-static-md` CRT-mismatch trap does **not** apply here.
- Because the engine links that import library, **every** executable linking OloEngine — including
  `OloServer` and `OloEngine-Tests`, which never call Steam — needs `steam_api64.dll` beside it at
  process load. The build stages it automatically; if you move a binary by hand, take the DLL too.

**CI never builds this path**, because the SDK cannot reach a public repo's runners. Instead a
dedicated workflow builds the Steam path against hand-written stub headers
(`-DOLO_WITH_STEAM_STUB_SDK=ON`), which compiles, links and runs the enabled code without any SDK.
If you add a Steamworks call, add it to the stubs too or that job goes red — see
[.github/workflows/steam-stub.yml](../../.github/workflows/steam-stub.yml).

Building the SDK in is only half of shipping on Steam — actually publishing a build (depot layout,
`steamcmd` upload automation, build identity) is a separate, developer-run concern documented in
[shipping.md](shipping.md).

---

## Windows

### Compiler
Visual Studio 2026 is the default for the `msvc` CMake preset (`Visual Studio 18 2026`
generator, requires CMake 4.2+). Visual Studio 2022 (v17.x) is also fully supported
via `scripts/Win-GenerateProjectVS2022.bat`, which calls CMake directly without
the preset and therefore only requires CMake 3.25+.

### Generate & Build
```batch
rem Generate VS solution
scripts\Win-GenerateProjectVS2022.bat
rem   — or —
scripts\Win-GenerateProjectVS2026.bat

rem Build from command line
cmake --build build --target OloEditor --config Debug --parallel
cmake --build build --target OloServer --config Debug --parallel
```

### VS Code Tasks
Use the pre-configured tasks in `.vscode/tasks.json`:
- `build-oloeditor-debug` / `build-oloeditor-release` / `build-oloeditor-dist`
- `run-oloeditor-debug` (working dir = `OloEditor/`)
- `build-tests-debug` / `run-tests-debug`
- `build-oloruntime-debug` / `run-oloruntime-debug`

### Working Directory
Always run OloEditor / OloRuntime from the `OloEditor/` directory — asset paths,
shader files, and Mono assemblies are resolved relative to it.

### C# Scripting (Mono)
C# scripting via Mono is built automatically on Windows **when using the Visual
Studio generator** (the `msvc` preset or `Win-GenerateProjectVS2022.bat` /
`Win-GenerateProjectVS2026.bat`) — `OloEditor` depends on the `OloEngine-ScriptCore`
and `Sandbox-Scripting` C# targets, so a plain `cmake --build build --target
OloEditor` also compiles them and places `OloEngine-ScriptCore.dll` directly into
`OloEditor/Resources/Scripts/`. The C# targets don't exist under the `clangcl` /
`clangcl-asan` presets (Ninja has no C#/MSBuild project support), so a build from
those presets has no C# scripting — this is expected, not a bug. The static Mono
libraries are bundled under `OloEngine/mono/lib/`.

---

## Linux

### System Requirements

The following development libraries and tools are required on Linux:

**Build tools:** C++23 compiler (GCC 14+), CMake 3.25+, Ninja, Git, pkg-config

**Graphics & windowing:** OpenGL (Mesa), X11 (libx11, libxrandr, libxinerama, libxcursor, libxi, libxext), Wayland (libwayland, wayland-protocols, libxkbcommon)

**Vulkan & shader compilation:** Vulkan headers/loader, shaderc, SPIRV-Cross, glslang, SPIRV-Tools

**Additional:** Python 3 with Jinja2 (for glad GL loader generation)

Most other dependencies (GLFW, GLM, Jolt, protobuf, libsodium, assimp, …) come from the
vcpkg manifest and are built once into the machine-global binary cache — see the vcpkg
prerequisite above; the Linux presets use vcpkg's stock `x64-linux` triplet. A handful
(ImGui, ImGuizmo, glad, Lua, sol2, stb, filewatch, …) are still fetched in-tree via FetchContent/CPM;
`OloEngine/vendor/CMakeLists.txt`'s header comment names each one and why it did not move.

Note vcpkg builds `libx11` and friends from source on Linux, so the *first* configure on
a machine is slow even with the system X11 dev packages installed; subsequent worktrees
restore from the binary cache.

#### Ubuntu 24.04 (and WSL)

```bash
# Build tools
sudo apt install -y gcc-14 g++-14 cmake ninja-build pkg-config

# Graphics & windowing
sudo apt install -y libgl-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
    libwayland-dev wayland-protocols libxkbcommon-dev

# Vulkan & shader compilation
sudo apt install -y libvulkan-dev glslang-dev libshaderc-dev \
    libspirv-cross-c-shared-dev spirv-tools glslc vulkan-tools

# Additional
sudo apt install -y python3-jinja2
```

Set GCC 14 as the default compiler (`CC`/`CXX` must be set before configuring):
```bash
export CC=gcc-14
export CXX=g++-14
```

#### Rocky / RHEL 10 (and other dnf-based distros)

On RHEL-family distros the shader stack is split across repos and **SPIRV-Cross
is not packaged at all**, so the simplest route is: install the GL/X11/Wayland dev
libs from dnf and get the whole shader stack (SPIRV-Cross, glslang, shaderc,
SPIRV-Tools) plus the Vulkan loader from the **LunarG Vulkan SDK** (installs to your
home dir, no root).

```bash
# Compiler — the latest packaged GCC is the gcc-toolset-15 SCL (GCC 15.x). The
# base `gcc gcc-c++` (GCC 14.x) also works and matches CI.
sudo dnf install -y gcc-toolset-15          # or: sudo dnf install -y gcc gcc-c++
source /opt/rh/gcc-toolset-15/enable 2>/dev/null \
    || export PATH=/opt/rh/gcc-toolset-15/root/usr/bin:$PATH   # if there's no enable script

# Graphics & windowing (X11 headers are needed to build the vendored GLFW)
sudo dnf install -y mesa-libGL-devel \
    libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel \
    wayland-devel wayland-protocols-devel libxkbcommon-devel

# CMake 4.2+ and Ninja (the packaged cmake is too old for CMakePresets), plus
# Jinja2 for the glad GL-loader generator — via pip, no root.
python3 -m ensurepip --user        # only if pip is missing
python3 -m pip install --user "cmake>=4.2" ninja jinja2

# Vulkan SDK — bundles SPIRV-Cross (absent from dnf), glslang, shaderc,
# SPIRV-Tools and the Vulkan loader. Installs to your home dir, no root.
curl -L https://sdk.lunarg.com/sdk/download/latest/linux/vulkan_sdk.tar.xz -o ~/vulkan_sdk.tar.xz
mkdir -p ~/vulkan-sdk && tar -xf ~/vulkan_sdk.tar.xz -C ~/vulkan-sdk
source ~/vulkan-sdk/*/setup-env.sh   # sets VULKAN_SDK
```

Then configure with the **`linux-gcc-toolset`** preset (it uses `gcc`/`g++` from
PATH instead of the Ubuntu-style `gcc-14` names, and turns FFmpeg off — see notes):

```bash
cmake --preset linux-gcc-toolset -DCMAKE_PREFIX_PATH="$VULKAN_SDK"
cmake --build build-linux --target OloEngine-Tests --config Debug --parallel
./build-linux/OloEngine/tests/Debug/OloEngine-Tests --gtest_filter='NetworkLobby.*:NetworkSession.*'
```

**RHEL/Rocky-specific notes:**
- **FFmpeg** (`OLO_VIDEO_FFMPEG`, on by default) builds from source and needs
  `nasm` (`sudo dnf install -y nasm`). The `linux-gcc-toolset` preset turns it
  **off** (the engine falls back to the pl_mpeg MPEG-1 decoder); re-enable with
  `-DOLO_VIDEO_FFMPEG=ON` once nasm is installed.
- **`libstdc++exp`** (which holds `std::stacktrace`) is **not shipped by
  gcc-toolset-15**. The build auto-detects a compatible copy under the base GCC
  install (`/usr/lib/gcc/*/*/libstdc++exp.a`) and links it. If configure warns that
  it wasn't found, install `libstdc++-static` (or the base `gcc`).
- The distro `vulkan-loader-devel` package is only the loader — it does **not**
  include SPIRV-Cross. Use the LunarG SDK above (or build SPIRV-Cross from source).
- The `linux-gcc-toolset` preset shares `build-linux/` with `linux-gcc`; use one
  or the other, not both against the same directory.

### Vulkan SDK Environment

Ensure `VULKAN_SDK` is set before configuring:
```bash
# If installed via package manager, the SDK is typically at:
export VULKAN_SDK=/usr
# Or for manual install:
# source /path/to/vulkan-sdk/setup-env.sh
```

### Configure & Build

```bash
# Configure (from project root)
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Build the headless server
cmake --build build --target OloServer --parallel

# Build the full editor (requires display server)
cmake --build build --target OloEditor --parallel

# Build and run tests
cmake --build build --target OloEngine-Tests --parallel
./build/OloEngine/tests/OloEngine-Tests
```

### WSL (Windows Subsystem for Linux)

WSL can be used to **compile** all targets and **run OloServer** (headless). However,
**OloEditor and OloRuntime are not supported under WSL** — WSL2's Mesa `llvmpipe`
software renderer only exposes OpenGL 4.5, while the engine requires OpenGL 4.6
with DSA. Use a native Linux installation or the Windows build for the editor.

### Linux Platform Notes

| Feature           | Status           | Notes                                              |
|-------------------|------------------|----------------------------------------------------|
| OloServer (headless)| Supported     | Primary Linux target — no GPU required             |
| OloEditor         | Supported        | Requires X11/Wayland display and OpenGL 4.6 GPU   |
| OloEditor on WSL  | Not supported    | WSL2 llvmpipe only provides OpenGL 4.5            |
| C# scripting (Mono)| Not yet available| Mono is Windows-only for now; Lua scripting works  |
| Lua scripting     | Supported        | Works identically to Windows                       |

### Running

```bash
# Headless server
cd OloEditor && ../bin/Debug/OloServer/OloServer

# Editor (requires GPU)
cd OloEditor && ../bin/Debug/OloEditor/OloEditor
```

---

## Build Targets

| Target                    | Description                                                                |
|---------------------------|----------------------------------------------------------------------------|
| `OloEngine`               | Core engine static library                                                 |
| `OloEditor`               | ImGui-based editor application                                             |
| `OloRuntime`              | Standalone game runtime                                                    |
| `OloServer`               | Headless dedicated server                                                  |
| `OloEngine-Tests`         | GoogleTest test suite                                                      |
| `OloEngine-LuaScriptCore` | Lua / Sol2 scripting bindings (built on all platforms)                     |
| `OloEngine-ScriptCore`    | C# / Mono scripting bindings (Visual Studio generator only — Windows path) |
| `GenerateBindings`        | Custom target that runs `OloHeaderTool` to regenerate C++ / C# glue        |

---

## Selecting the renderer backend (`--rhi=`)

One binary carries both backends (ADR 0011 §2 — a runtime switch, deliberately
not a CMake preset). Selection order, first hit wins:

1. `--rhi=opengl` / `--rhi=vulkan` on the command line (case-insensitive).
2. `config/renderer.yaml` (`Renderer: { RHI: <name> }`) — looked up under the
   process working directory first, then next to the executable (#691;
   the exe-dir fallback is what rescues a packaged game launched
   with a stale shortcut "Start in"). The editor's **Renderer Settings**
   dropdown writes this file; a packaged game ships one from the Build Game
   pipeline (`GameBuildSettings::DefaultRendererBackend`). Applies on
   restart, never live.
3. OpenGL.

An unknown name, or `vulkan` in an `OLO_WITH_VULKAN=OFF` binary, degrades to
OpenGL loudly. A **config-sourced** Vulkan selection that fails device init
retries on OpenGL and rewrites the file (a persisted preference must not
brick the install); an explicit `--rhi=vulkan` flag keeps ADR 0010's
refuse-to-init behaviour so capability failures stay visible. The active
backend is logged at startup and, when it isn't the GL default, shown in the
window title. `OloServer` is headless and selects no backend at all (ADR
0011 amendment (86)).

### `OLO_WITH_VULKAN=OFF` — what it does and does not do

The *build-time* axis, distinct from `--rhi=` above. `-DOLO_WITH_VULKAN=OFF`
drops the `volk` / `vulkan-headers` / `vulkan-memory-allocator` vcpkg ports and
compiles every `OloEngine/src/Platform/Vulkan/*.cpp` to an empty object, so the
binary ships the OpenGL backend only.

Two things it is **not**, both of which the docs claimed until #811:

- **It is not "build without Vulkan development packages."**
  `find_package(Vulkan REQUIRED ...)` in `OloEngine/CMakeLists.txt` is ungated on
  purpose, because the *shader* toolchain (shaderc, glslang, SPIRV-Tools,
  SPIRV-Cross) is needed by every build regardless of backend. Turning the option
  off does not relax that, but what satisfies it differs by platform:
  - **MSVC** takes the `COMPONENTS glslc glslang SPIRV-Tools` branch, which in
    practice means the LunarG SDK — configuring without `VULKAN_SDK` fails at
    `find_package`, whichever way this option is set.
  - **Everywhere else** takes a plain `find_package(Vulkan REQUIRED)`, so any
    discoverable Vulkan installation will do and the shader tooling can come from
    system packages instead (`glslang-dev`, `libshaderc-dev`,
    `libspirv-cross-c-shared-dev`, `spirv-tools` on Debian/Ubuntu — the same set
    the Linux CI jobs install). No LunarG SDK is strictly required there.
- **It is not per-target.** `OloServer`, `OloEditor` and `OloRuntime` come out of
  one configure, so there is no "OFF for the server, ON for the editor". A lean,
  SDK-free server build was considered in #811 and explicitly **rejected**: the
  server is backend-less already, and its deployment image (ubuntu +
  libstdc++6 — see [deployment.md](deployment.md)) carries no GPU stack, so the
  build weight the valve saves buys nothing there.

What it *is* good for: cutting three ports and the backend object code out of a
GL-only build. It is exercised in CI by
[`.github/workflows/vulkan-off.yml`](../../.github/workflows/vulkan-off.yml),
which configures + builds it on Linux and asserts the Vulkan test suites SKIP
cleanly. Run it locally with a separate build directory so it does not clobber
the default tree:

```powershell
cmake --preset msvc -B build-vkoff -DOLO_WITH_VULKAN=OFF
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-vkoff --target OloEngine-Tests --config Debug --parallel 6'
```

### `OLO_WITH_FSR2` — the FSR2 temporal upscaler (Windows + OpenGL only)

Defaults **ON, on Windows only**; `cmake/fsr2.cmake` forces it OFF everywhere else and says so at
configure time. When ON it fetches
[JuanDiegoMontoya/FidelityFX-FSR2-OpenGL](https://github.com/JuanDiegoMontoya/FidelityFX-FSR2-OpenGL)
(pinned to a commit SHA, submodules deliberately **not** cloned — upstream registers a
multi-gigabyte art repo for its sample app), compiles its SPIR-V shader permutations with the
prebuilt `tools/sc/FidelityFX_SC.exe` from that tree into `<buildDir>/fsr2-gl-shaders/`, and builds
the `ffx_fsr2_gl` static library.

**The Windows restriction is structural, not a to-do.** The upstream OpenGL backend uses
`wcstombs_s`, `GetModuleHandleA` and `<Windows.h>`, and its shader compiler is a prebuilt Win32
binary with no Linux build. Nothing else changes: `Platform/OpenGL/OpenGLTemporalUpscaler.cpp`
self-guards on the macro and compiles to a stub, the render pass and its settings compile and are
tested on every platform, and a build with the option off simply reports the temporal upscaler as
unavailable and uses the FSR1 spatial upscaler instead (which keeps the chosen render scale).

Turn it off explicitly if you want to skip the ~60 MB fetch:

```powershell
cmake --preset dev-cached -DOLO_WITH_FSR2=OFF
```

The first configure after enabling it pays the clone; the permutation compile is an
`add_custom_command` keyed on each pass source **and** on every shared `ffx_*.h` shader header
(`OLO_FSR2_SHADER_INCLUDES`), so it re-runs when either changes and is otherwise paid once per build
tree.

---

## Build speed options

The build is tuned for fast compiles out of the box. Four CMake options control the levers;
the defaults are what you want for local dev.

| Option                     | Default | Helps               | Notes                                                         |
|----------------------------|---------|---------------------|--------------------------------------------------------------|
| `OLO_ENABLE_PCH`           | `ON`    | cold **and** warm   | Precompiles `OloEnginePCH.h`; PUBLIC, so editor/runtime/tests inherit it. This is the single biggest cold-build lever. |
| `OLO_ENABLE_LTO`           | `ON`    | runtime perf        | Release/Dist only (it would cripple Debug link times).       |
| `OLO_ENABLE_COMPILER_CACHE`| `OFF`   | warm/incremental    | sccache/ccache; Ninja-only (the VS generator ignores compiler launchers). CI's win. Force-disables PCH **and** unity (both are non-cacheable). |
| `OLO_ENABLE_UNITY_BUILD`   | `OFF`   | cold (situationally)| Jumbo/unity build of `OloEngine`. See below — measured, marginal. |

Two further options are safety rails rather than speed levers. `OLO_ARCHIVE_WARN_PERCENT` (75) and
`OLO_ARCHIVE_FAIL_PERCENT` (90) bound how close a static archive may get to the COFF format's hard
4 GiB ceiling; `cmake/CheckArchiveSize.cmake` prints each archive's headroom after every build and
fails before `lib.exe` would. The engine ships as **three** archives (`OloEngine`,
`OloEngineRenderer`, `OloEngineContent`) for exactly this reason — with `OLO_ENABLE_LTO=ON` a
single one measured 119% of the limit and nothing could link in Dist. If the guard fires, cut the
offending part further rather than raising the threshold:
[static-archive-4gib-ceiling.md](../agent-rules/static-archive-4gib-ceiling.md).

### Unity (jumbo) builds — `-DOLO_ENABLE_UNITY_BUILD=ON`

Batches the `OloEngine` TUs 16-per-jumbo (`UNITY_BUILD_BATCH_SIZE`) so headers parse once per
batch instead of once per file. **Opt-in and OFF by default**, and it auto-disables when the
compiler cache is on (a one-line edit busts the whole 16-file jumbo's cache key — a net loss on
warm-cache incremental builds; see `cmake/CompilerCache.cmake`). CI uses the cache, so CI never
runs unity.

**Measured cold-build result (isolated `OloEngine` recompile, vendor warm, Debug, MSVC, 28-core,
cache OFF):** across 11 runs the median was **identical at 336s** ON vs OFF; the best run favoured
ON (220s vs 275s, ~20%) and ON won 3 of 4 load-matched pairs, but one pair favoured OFF. So unity
is **a modest, situational cold-build win at best, easily masked by machine load.** The reason it
isn't bigger: **PCH already amortizes the header-parsing cost that unity targets**, so they don't
stack — and on a many-core machine the 56 coarse unity TUs schedule across cores slightly worse
than the ~470 fine ones. Unity is most likely to pay off on a **core-starved machine without
PCH** — which OloEngine is not. **Recommendation: leave it OFF unless you're on a low-core machine
doing repeated cold builds and have measured a win for your hardware.**

Enabling it required two build-only changes (no engine-logic edits): `Renderer/Vertex.h` no longer
pulls the unused experimental `<glm/gtx/integer.hpp>` (which `#error`'d in jumbos before
`GLM_ENABLE_EXPERIMENTAL` was active), and 27 TUs are excluded from batching via
`SKIP_UNITY_BUILD_INCLUSION` in `OloEngine/src/CMakeLists.txt` — 7 third-party single-header
amalgamations (miniaudio/stb/pl_mpeg/ImGui/FFmpeg) plus 20 engine TUs whose copy-pasted
file-local helpers (`IsTruthyEnvironmentVariable`, `SafeNormalize`, `kTwoPi`, …) collide when
concatenated. De-duplicating those helpers into shared headers would let them rejoin the batches;
it's deliberately left as a follow-up so this stays a low-risk, CMake-only change.

---

## Troubleshooting

### Missing shaders / Mono assemblies at runtime
Ensure the working directory is `OloEditor/` when running the editor or server.

### `VULKAN_SDK` not set
Set the environment variable before running CMake. On Linux with the Vulkan SDK
installed via the package manager, `export VULKAN_SDK=/usr` is usually sufficient.

### GLFW build errors on Linux
Install the X11/Wayland development packages listed above. GLFW needs the display
server headers at compile time.

### Mono not found on Linux
C# scripting is currently Windows-only. The engine builds without it on Linux.
Lua scripting is fully functional on all platforms.

### libsodium version.h not found
On MSVC, the vendor build copies the pre-existing `version.h` from
`builds/msvc/version.h` into the include directory. On non-MSVC builds (GCC,
Clang), CMake generates `version.h` at
`src/libsodium/include/sodium/version.h` during the configure step. If the
header is missing, rerun the CMake configure step (`cmake -B build ...`) and
check that the generated file exists under the FetchContent source directory.
