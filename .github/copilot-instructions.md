Stick to core OpenGL (4.5+) with GLSL shaders, using SPIR-V.
Minimize state changes and efficiently manage shader programs.
Optimize the rendering loop to reduce draw calls.
Manage resources efficiently.
Use C++ RAII patterns (e.g., smart pointers).
Focus on real-time performance.
Keep GPU–CPU sync minimal and avoid unnecessary stall.
Favor STL containers (e.g., std::vector) for contiguous memory and cache friendliness. Pre-allocate or reuse memory to reduce dynamic allocation overhead.
Use custom allocators for frequent small allocations and ensure proper alignment for SIMD optimizations.

# Code Style Guidelines
Naming: PascalCase for classes, m_PascalCase for member variables, s_PascalCase for static variables.
Constants/Macros: UPPER_CASE with OLO prefix (OLO_ASSERT, OLO_CORE_ERROR).
Types: Custom typedefs for primitives (u8, i16, i32, f32, etc.).
Header Files: Use pragma once, not include guards.
Error Handling: Use OLO_ASSERT, OLO_CORE_ASSERT for validation and logging macros for info/warnings.
Formatting: Braces on new lines, 4-space indentation, public methods before private.
Memory Management: Use CreateScope<T> and CreateRef<T> helpers (unique_ptr/shared_ptr).
Namespaces: All engine code wrapped in the OloEngine namespace.
Documentation: Provide clear comments for complex functionality; use TODO markers for future improvements.
Design Patterns: Adopt an interface/implementation split for platform-specific functionality.
