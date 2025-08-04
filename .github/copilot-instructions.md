# GitHub Copilot Instructions

Only add comments if the code is not self-explanatory.
Avoid feature creep; stick to adding only the requested functionality.
Keep it simple and focused on the task at hand.
Use modern C++ features (C++17 and later, ideally C++20/C++23).
Use modern OpenGL (4.5+) with Direct State Access (DSA) for better performance and cleaner code.
Use RAII principles for resource management (e.g., OpenGL resources, file handles).
Favor STL containers (e.g., std::vector) for contiguous memory and cache friendliness. Pre-allocate or reuse memory to reduce dynamic allocation overhead.

# Code Style Guidelines

- **Naming:** PascalCase for classes, m_PascalCase for member variables, s_PascalCase for static variables.

- **Types:** Custom typedefs for primitives (u8, i16, i32, f32, sizet, etc.).

- **Header Files:** Use pragma once, not include guards.

- **Formatting:** Braces on new lines, 4-space indentation, public methods before private.

- **Memory Management:** Use AssetRef<T> for asset objects and CreateScope<T> for non-asset objects.

- **Namespaces:** All engine code wrapped in the OloEngine namespace.

## VS Code Tasks Usage
When running or testing any application (e.g., Sandbox3D, OloEditor, Sandbox2D), always use the corresponding VS Code task defined in .vscode/tasks.json (such as 'run-sandbox3d-debug', 'run-oloeditor-release', etc.) instead of launching binaries directly. This ensures the correct working directory and environment are set. Do not use direct binary execution for starting or testing applications in this repository.
