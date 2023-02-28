# OloEngine
OloEngine is primarily an early-stage interactive application and rendering engine for Windows based on [Hazel](https://github.com/TheCherno/Hazel/).

## Getting Started
Requirements:
- Python 3.10+, with the 'jinja2' package installed (needed for building glad2)
- CMake 3.22+
- Vulkan-SDK (for SPIR-V)

You can clone the repository to a local destination using git:

`git clone https://github.com/drsnuggles8/OloEngineBase`

This project uses [CMake](https://cmake.org/download/) to build the project's solution files. There's a batch script `scripts/Win-GenerateProject.bat` that will generate the solution file for Visual Studio 2022.
If you're using CLion, just open the OloEngineBase folder, let CLion initialize the CMake project and then edit the run configurations by changing the working directory of the OloEditor application to be the OloEditor folder. 

CMake downloads all dependencies via the `Fetchcontent_Declare()` function, and stores them in the OloEngine/vendor directory.
CMake will also create the build directory, which contains the Visual Studio solution files.

## The Plan
The plan for OloEngine is to create a 2D engine by following the Hazel videos, and then expanding to 3D graphics while implementing our own desired features.
We aim to use modern C++ 20, implementing modules as soon as they're properly supported in Visual Studio (hopefully coming soon).

### Main features to come
-   Fast 2D rendering (UI, particles, sprites, etc.)
-   High-fidelity physically-based 3D rendering (this will be expanded later, 2D to come first)
-   Native rendering API support (OpenGL / Vulkan)
-   Fully featured viewer and editor applications
-   Fully scripted interaction and behavior
-   Integrated 3rd party 2D and 3D physics engine
-   Procedural terrain and world generation
-   Artificial Intelligence
-   Audio system
-   Smart procedural generation
-   Asset management system
-   Networking capabilities
-   Generation of art assets through deep learning 

## Dependencies
 * [assimp](https://github.com/assimp/assimp) : A library to import and export various 3d-model-formats including scene-post-processing to generate missing render data.
 * [box2d](https://github.com/erincatto/Box2D) : 2D physics engine.
 * [entt](https://github.com/skypjack/entt) : Fast and reliable entity-component system (ECS)
 * [filewatch](https://github.com/ThomasMonkman/filewatch) : Single header folder/file watcher in C++11 for windows and linux, with optional regex filtering
 * [glad](https://github.com/Dav1dde/glad) : Meta loader for OpenGL API.
 * [glfw](https://github.com/glfw/glfw) : A multi-platform library for OpenGL, OpenGL ES, Vulkan, window and input.
 * [glm](https://github.com/g-truc/glm) : OpenGL Mathematics (GLM) is a header only C++ mathematics library for graphics software based on the OpenGL Shading Language (GLSL) specifications.
 * [googletest](https://github.com/google/googletest) : Google Testing and Mocking Framework.
 * [miniaudio](https://github.com/mackron/miniaudio) : Audio playback and capture library written in C, in a single source file. 
 * [imgui](https://github.com/ocornut/imgui) : Dear ImGui: Bloat-free Immediate Mode Graphical User interface for C++ with minimal dependencies.
 * [imguizmo](https://github.com/CedricGuillemet/ImGuizmo) : Immediate mode 3D gizmo for scene editing and other controls based on Dear Imgui.
 * [spdlog](https://github.com/gabime/spdlog) : Fast C++ logging library.
 * [stb](https://github.com/nothings/stb) : Single-file public domain (or MIT licensed) libraries for C/C++.
 * [sol2](https://github.com/ThePhD/sol2) : C++ <-> Lua API wrapper
 * [tracy](https://github.com/wolfpld/tracy) : A real time, nanosecond resolution, remote telemetry, hybrid frame and sampling profiler for games and other applications.
 * [yaml-cpp](https://github.com/jbeder/yaml-cpp) : yaml-cpp is a YAML parser and emitter in C++ matching the YAML 1.2 spec.

 ## Influences
  * [Hazel](https://github.com/TheCherno/Hazel) : As already mentioned, this game engine is following The Cherno's game engine series.
  * [Lumos](https://github.com/jmorton06/Lumos) : For ideas on OpenGL's rendering implementation.
  * [Arc](https://github.com/MohitSethi99/ArcGameEngine) : For ideas on the audio engine.
