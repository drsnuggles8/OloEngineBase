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
| Git         | 2.x             | FetchContent clones vendor deps        |
| Vulkan SDK  | 1.3+            | `VULKAN_SDK` env var must be set       |
| C++ compiler| C++23 support   | Known-working: MSVC 17.x / GCC 14+ / Clang 17+. CMake enforces `CMAKE_CXX_STANDARD = 23` (required) but does not enforce specific compiler versions; older compilers with full C++23 support may work but are untested. |

The Vulkan SDK must include `glslc` and `glslangValidator`.

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
C# scripting via Mono is built automatically on Windows. The static Mono libraries
are bundled under `OloEngine/mono/lib/`.

---

## Linux

### System Requirements

The following development libraries and tools are required on Linux:

**Build tools:** C++23 compiler (GCC 14+), CMake 3.25+, Ninja, Git, pkg-config

**Graphics & windowing:** OpenGL (Mesa), X11 (libx11, libxrandr, libxinerama, libxcursor, libxi, libxext), Wayland (libwayland, wayland-protocols, libxkbcommon)

**Vulkan & shader compilation:** Vulkan headers/loader, shaderc, SPIRV-Cross, glslang, SPIRV-Tools

**Additional:** Python 3 with Jinja2 (for glad GL loader generation)

Most other dependencies (GLFW, ImGui, GLM, entt, Jolt, protobuf, libsodium, etc.) are fetched automatically via CMake FetchContent.

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

## Build speed options

The build is tuned for fast compiles out of the box. Four CMake options control the levers;
the defaults are what you want for local dev.

| Option                     | Default | Helps               | Notes                                                         |
|----------------------------|---------|---------------------|--------------------------------------------------------------|
| `OLO_ENABLE_PCH`           | `ON`    | cold **and** warm   | Precompiles `OloEnginePCH.h`; PUBLIC, so editor/runtime/tests inherit it. This is the single biggest cold-build lever. |
| `OLO_ENABLE_LTO`           | `ON`    | runtime perf        | Release/Dist only (it would cripple Debug link times).       |
| `OLO_ENABLE_COMPILER_CACHE`| `OFF`   | warm/incremental    | sccache/ccache; Ninja-only (the VS generator ignores compiler launchers). CI's win. Force-disables PCH **and** unity (both are non-cacheable). |
| `OLO_ENABLE_UNITY_BUILD`   | `OFF`   | cold (situationally)| Jumbo/unity build of `OloEngine`. See below — measured, marginal. |

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
