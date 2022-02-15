# OloEngine

OloEngine is primarily an early-stage interactive application and rendering engine for Windows based on [Hazel](https://github.com/TheCherno/Hazel/).

## Getting Started
Visual Studio 2022 is recommended, OloEngine is officially untested on other development environments whilst we focus on a Windows build.

You can clone the repository to a local destination using git:

`git clone https://github.com/drsnuggles8/OloEngine`

This project uses CMake to build the solution files. There's a batch file that you can run to generate the solution file for Visual Studio 2022 (although you probably have to change the directories used).
Alternatively, you can run the command to run cmake (again, adjust the folders to your needs):

`cmake -Hc:/Users/ole/source/repos/OloEngineBase -Bc:/Users/ole/source/repos/OloEngineBase/build -G "Visual Studio 17 2022" -DCMAKE_GENERATOR_PLATFORM=x64`

[CMake](https://cmake.org/download/) will download all dependencies via the `Fetchcontent_Declare()` function, and store them in the OloEngine/vendor directory, while also creating the build directory, which contains the Visual Studio solution files. You can also let the build files be generated from inside Visual Studio Code with the CMake extension.

If you want to disable the automatic downloading, consider editing the CMakeLists.txt file in the root directory, and setting `FETCHCONTENT_FULLY_DISCONNECTED` to `ON` in line 25.

## The Plan
The plan for OloEngine is to create a 2D engine by following the Hazel videos, and then expanding to 3D graphics while implementing our own desired features.
We aim to use modern C++ 20, implementing modules as soon as they're properly supported in Visual Studio

### Main features to come:
- Fast 2D rendering (UI, particles, sprites, etc.)
- High-fidelity physically-based 3D rendering (this will be expanded later, 2D to come first)
- Native rendering API support (Vulkan)
- Fully featured viewer and editor applications
- Fully scripted interaction and behavior
- Integrated 3rd party 2D and 3D physics engine
- Procedural terrain and world generation
- Artificial Intelligence
- Audio system
- Smart procedural generation
- Asset management system
- Networking capabilities
