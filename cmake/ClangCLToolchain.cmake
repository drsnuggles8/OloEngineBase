# ClangCLToolchain.cmake
# Toolchain file for building with clang-cl (Clang with MSVC ABI) via Ninja.
# Used by the "clangcl" CMake preset.

set(CMAKE_C_COMPILER "clang-cl" CACHE STRING "")
set(CMAKE_CXX_COMPILER "clang-cl" CACHE STRING "")
set(CMAKE_LINKER "lld-link" CACHE STRING "")

# Suppress errors for MSVC-only flags (e.g. /experimental:c11atomics) that
# vendor libraries pass but clang-cl doesn't recognize.
set(CMAKE_C_FLAGS_INIT "-Wno-error=unused-command-line-argument")
set(CMAKE_CXX_FLAGS_INIT "-Wno-error=unused-command-line-argument")
