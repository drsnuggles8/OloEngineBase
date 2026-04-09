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
| CMake       | 3.25            | Required by root CMakeLists.txt        |
| Git         | 2.x             | FetchContent clones vendor deps        |
| Vulkan SDK  | 1.3+            | `VULKAN_SDK` env var must be set       |
| C++ compiler| C++23 support   | MSVC 17.x / GCC 14+ / Clang 17+       |

The Vulkan SDK must include `glslc` and `glslangValidator`.

---

## Windows

### Compiler
Visual Studio 2022 (v17.x) or Visual Studio 2026 with C++23 support.

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

| Target            | Description                                    |
|-------------------|------------------------------------------------|
| `OloEngine`       | Core engine static library                     |
| `OloEditor`       | ImGui-based editor application                 |
| `OloRuntime`      | Standalone game runtime                        |
| `OloServer`       | Headless dedicated server                      |
| `OloEngine-Tests` | GoogleTest test suite                          |

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
The vendor build copies `version.h` automatically for MSVC. On Linux with GCC,
ensure the libsodium FetchContent download completed (check `OloEngine/vendor/libsodium-src/`).
