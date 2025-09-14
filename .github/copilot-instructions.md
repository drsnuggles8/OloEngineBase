# GitHub Copilot Instructions

Prefer self-explanatory code, but document non-obvious decisions, invariants, ownership/lifetime, threading/concurrency assumptions, and performance trade-offs.
Avoid feature creep; stick to adding only the requested functionality.
Keep it simple and focused on the task at hand.Use modern C++ features (C++17 and later, ideally C++20/C++23).
Baseline: C++20 across the repo. C++23 features allowed if guarded by feature-detection and CI/toolchain support.Use RAII principles for resource management (e.g., OpenGL resources, file handles).
Favor STL containers (e.g., std::vector) for contiguous memory and cache friendliness. Pre-allocate or reuse memory to reduce dynamic allocation overhead.

# Code Style Guidelines

- **Naming:** PascalCase for classes, m_PascalCase for member variables, s_PascalCase for static variables.

- **Types:** Custom typedefs for primitives (u8, i16, i32, f32, sizet, etc.).

- **Header Files:** Use pragma once, not include guards.

- **Formatting:** Braces on new lines, 4-space indentation, public methods before private.

- **Formatting:** Braces on new lines, 4-space indentation, public before private. Enforced by a repo-wide .clang-format and pre-commit hook; CI rejects non-conforming diffs.
## VS Code Tasks Usage
When running or testing any application (e.g., Sandbox3D, OloEditor, Sandbox2D), always use the corresponding VS Code task defined in .vscode/tasks.json (such as 'run-sandbox3d-debug', 'run-oloeditor-release', etc.) instead of launching binaries directly. This ensures the correct working directory and environment are set. Do not use direct binary execution for starting or testing applications in this repository.
