# OloEngine
OloEngine is primarily an early-stage cross-platform (Windows and Linux) interactive application and rendering engine based on [Hazel](https://github.com/TheCherno/Hazel/).

## Screenshots

### 3D Physics Integration
![3D Physics](assets/3d_physics.png)

### Animated Models
![Animated Models](assets/animated_models.png)

### PBR Material Rendering
![PBR Model](assets/pbr_model.png)

## Getting Started

**Supported platforms:** Windows (Visual Studio 2026 via the default `msvc` preset; Visual Studio 2022 also supported via `scripts/Win-GenerateProjectVS2022.bat`), Linux (GCC 14+).
WSL can compile all targets and run OloServer, but OloEditor requires a native
OpenGL 4.6 GPU (WSL2's software renderer only supports OpenGL 4.5).

Requirements:
- Python 3.10+, with the 'jinja2' package installed (needed for building glad2)
- CMake 4.2+ (required by `CMakePresets.json`; the root `CMakeLists.txt` itself only requires 3.25, so plain `cmake -B build -G "Visual Studio 17 2022"` still works without presets)
- Vulkan SDK (for SPIR-V shader compilation)

You can clone the repository to a local destination using git:

`git clone https://github.com/drsnuggles8/OloEngineBase`

This project uses [CMake](https://cmake.org/download/) to build the project's solution files. The `scripts/` directory contains `Win-GenerateProjectVS2022.bat` and `Win-GenerateProjectVS2026.bat` helpers.

**Visual Studio Code Users:** The project includes predefined VS Code tasks for building and running. Use tasks like `build-oloeditor-debug`, `run-oloeditor-release`, etc. from the Command Palette (Ctrl+Shift+P → "Tasks: Run Task").

**CLion Users:** Open the OloEngineBase folder, let CLion initialize the CMake project, then edit the run configurations by changing the working directory of the OloEditor application to be the OloEditor folder.

CMake downloads all dependencies via FetchContent and CPM (CMake Package Manager) and stores them in the `OloEngine/vendor/` directory.
CMake will also create the `build/` directory, which contains the Visual Studio solution files.

## Current Features

### Core Engine
- **Entity-Component-System (ECS)**: Built on EnTT for high-performance entity management
- **Multi-threaded Asset System**: Async loading with hot-reload support via filewatch
- **Dual Scripting Support**: C# scripting via Mono integration + Lua scripting via Sol2
- **Comprehensive Serialization**: YAML-based scene and entity serialization
- **Advanced Memory Management**: Custom allocators and RAII resource management

### Rendering
- **OpenGL-based Renderer**: Modern OpenGL 4.6 with SPIR-V shader compilation
- **Physically-Based Rendering (PBR)**: Advanced material system with metallic-roughness workflow
- **Multi-threaded Command Queue**: Stateless, layered rendering architecture based on [Molecular Matters' design](https://blog.molecular-matters.com/2014/11/06/stateless-layered-multi-threaded-rendering-part-1/)
- **2D/3D Rendering Support**: Unified pipeline for both 2D sprites and 3D models
- **Animated Models**: Skeletal animation system with Assimp integration

### Physics
- **3D Physics**: Jolt Physics integration for rigid body dynamics
- **2D Physics**: Box2D integration for 2D game mechanics
- **Custom Collision Layers**: Flexible collision filtering and detection

### Editor & Tools
- **Full-Featured Editor**: ImGui-based editor with scene hierarchy and content browser
- **Visual Scene Editing**: Gizmos, transform tools, and real-time scene manipulation
- **Asset Management**: Comprehensive asset browser with drag-and-drop functionality
- **Profiling & Debugging**: Tracy profiler integration with custom renderer profilers
- **Memory Tracking**: Real-time GPU/CPU memory usage visualization

### Audio
- **3D Audio System**: Miniaudio-based with spatial audio support
- **Audio Components**: Source and listener components for ECS integration

### UI System
- **ECS-Based UI**: 16 widget component types (Canvas, RectTransform, ResolvedRect, Panel, Text, Image, Button, Slider, Checkbox, Toggle, Progress Bar, Input Field, Dropdown, Scroll View, Grid Layout, WorldAnchor)
- **Anchor Layout**: RectTransform-style anchoring, pivot, and auto-layout via grid containers
- **Editor Integration**: "Create UI" context menu, per-component property panels, editor-time preview in both 2D and 3D modes
- **Scripting Support**: Full C# (Mono) and Lua (Sol2) bindings for all UI components

## Future Features
- Advanced lighting (global illumination, ray tracing)
- Scripting debugger and hot-reload
- Additional compute-shader workloads (GPU culling fast paths, particle compute)
- Expanded post-processing pipeline (motion blur, additional AA modes)

## Dependencies

All dependencies are automatically fetched via fetchcontent and CPM (CMake Package Manager, only used for Sol) and stored in `OloEngine/vendor/`:

### Core Libraries
* [entt](https://github.com/skypjack/entt) - Fast and reliable entity-component system (ECS)
* [glm](https://github.com/g-truc/glm) - OpenGL Mathematics library for graphics transformations
* [spdlog](https://github.com/gabime/spdlog) - Fast C++ logging library
* [yaml-cpp](https://github.com/jbeder/yaml-cpp) - YAML parser and emitter for serialization
* [nlohmann/json](https://github.com/nlohmann/json) - JSON parsing used for tools and IPC
* [choc](https://github.com/Tracktion/choc) - Header-only C++ utility library (audio, threading, etc.)
* [atomic_queue](https://github.com/max0x7ba/atomic_queue) - Lock-free MPMC queue
* [meshoptimizer](https://github.com/zeux/meshoptimizer) - Mesh simplification and indexing

### Rendering & Graphics
* [glad](https://github.com/Dav1dde/glad) - OpenGL loader and meta loader
* [glfw](https://github.com/glfw/glfw) - Multi-platform library for OpenGL, window and input
* [assimp](https://github.com/assimp/assimp) - 3D model importing with scene post-processing
* [stb](https://github.com/nothings/stb) - Single-file public domain libraries (stb_image for textures)
* [zlib](https://www.zlib.net/) - Compression library

### Physics
* [joltphysics](https://github.com/jrouwe/JoltPhysics) - Multi-platform 3D physics engine
* [box2d](https://github.com/erincatto/Box2D) - 2D physics engine for games

### Networking
* [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) - Reliable UDP transport
* [libsodium](https://doc.libsodium.org/) - Crypto primitives used by the transport
* [protobuf](https://github.com/protocolbuffers/protobuf) - Wire format for snapshots / RPC

### Navigation
* [recastnavigation](https://github.com/recastnavigation/recastnavigation) - Navigation mesh generation

### Audio
* [miniaudio](https://github.com/mackron/miniaudio) - Single-file audio playback and capture library

### Scripting
* [sol2](https://github.com/ThePhD/sol2) - C++ ↔ Lua API wrapper with advanced features
* [lua](https://www.lua.org/) - Lightweight, embeddable scripting language
* **Mono** - C# scripting runtime (manually integrated)

### UI & Editor
* [imgui](https://github.com/ocornut/imgui) - Immediate mode GUI for tools and debugging
* [imguizmo](https://github.com/CedricGuillemet/ImGuizmo) - Immediate mode 3D gizmo for scene editing

### Development & Profiling
* [tracy](https://github.com/wolfpld/tracy) - Real-time profiler with nanosecond resolution
* [googletest](https://github.com/google/googletest) - Google Testing and Mocking Framework
* [filewatch](https://github.com/ThomasMonkman/filewatch) - File system monitoring for hot-reload


### Code formatting & pre-commit hooks ✅

We trim trailing whitespace and enforce a few formatting rules via `pre-commit` hooks.

- Install pre-commit locally (Python/pip required):
  - `python -m pip install --user pre-commit`
  - `pre-commit install` in the repo root to enable the git hook
  - Optionally run `pre-commit run --all-files` to apply the hooks across the repo

Included hooks/config:
- `trailing-whitespace` (removes trailing spaces)
- `end-of-file-fixer` (ensure final newline)
- `clang-format` for C/C++ (configured to use `.clang-format` in the repo root)

We also include a `.editorconfig` to configure editor behavior (trim trailing whitespace, insert final newline). The pre-commit hooks **ignore the `vendor/`, `.vs/`, `.vscode/`, `build/`, and other generated/IDE folders** to avoid touching third-party or editor metadata.

A GitHub Action runs `pre-commit` on push and pull requests so formatting checks run on PRs automatically.


 ## Influences & References
  * [Hazel](https://github.com/TheCherno/Hazel) - As mentioned, this game engine follows The Cherno's game engine series as a foundation
  * [Lumos](https://github.com/jmorton06/Lumos) - Ideas for OpenGL rendering implementation and engine architecture
  * [Arc](https://github.com/MohitSethi99/ArcGameEngine) - Ideas for the audio engine implementation
  * [Molecular Matters Blog](https://blog.molecular-matters.com/2014/11/06/stateless-layered-multi-threaded-rendering-part-1/) - Core inspiration for the multi-threaded render command queue architecture
