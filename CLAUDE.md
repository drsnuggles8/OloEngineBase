# OloEngine Developer Guidelines

## Build Commands
- Generate project: `scripts/Win-GenerateProject.bat` or use CMake directly
- Build: Open generated solution in Visual Studio/CLion or run `cmake --build build`
- Run: Execute OloEditor with working directory set to OloEditor folder

## Code Style Guidelines
- Naming: PascalCase for classes, m_PascalCase for member variables, s_PascalCase for static variables
- Constants/Macros: UPPER_CASE with OLO prefix (OLO_ASSERT, OLO_CORE_ERROR)
- Types: Custom typedefs for primitives (u8, i16, i32, f32, etc.)
- Header Files: Use #pragma once for include guards
- Error Handling: Use OLO_ASSERT, OLO_CORE_ASSERT for validation, logging macros for info/warnings
- Formatting: Braces on new lines, 4-space indentation, public methods before private
- Memory Management: Use CreateScope<T> and CreateRef<T> helpers (unique_ptr/shared_ptr)
- Namespaces: All engine code wrapped in OloEngine namespace
- Documentation: Clear comments for complex functionality, TODO markers for future work
- Design Patterns: Interface/Implementation for platform-specific functionality