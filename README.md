# OloEngine
OloEngine is primarily an early-stage interactive application and rendering engine for Windows based on [Hazel](https://github.com/TheCherno/Hazel/).

## Getting Started
Requirements:
- Python 3.10 + jinja2 (for building glad2)
- CMake 3.22+

You can clone the repository to a local destination using git:

`git clone https://github.com/drsnuggles8/OloEngine`

This project uses [CMake](https://cmake.org/download/) to build the solution files. There's a batch script `scripts/Win-GenerateProject.bat` that will generate the solution file for Visual Studio 2022.
If you're using CLion, just open the OloEngineBase folder, let CLion initialize the CMake project and then edit the run configurations by changing the working directory of the OloEditor application to be the OloEditor folder. 

The batch script will download all dependencies via CMake's `Fetchcontent_Declare()` function, and store them in the OloEngine/vendor directory.
CMake will also creatie the build directory, which contains the Visual Studio solution files.

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
