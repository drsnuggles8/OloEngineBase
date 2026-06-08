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

# The clang-cl toolset makes CMake pick llvm-rc as the resource compiler. Unlike
# Microsoft's rc.exe, llvm-rc does NOT honour a .rc file's `#pragma code_page(N)`
# and assumes ASCII input, so it rejects any legacy code-page byte — e.g. assimp's
# version resource defines the copyright string as "\xA9 2006-..." (0xA9 = © in
# cp1252) and llvm-rc fails with "Non-ASCII 8-bit codepoint (169) can't occur in a
# non-Unicode string". rc.exe (used by the MSVC preset) compiles it fine. Tell
# llvm-rc the input code page explicitly; 1252 is the Western Windows ACP that
# rc.exe defaults to and that assimp's own `#pragma code_page(1252)` declares.
set(CMAKE_RC_FLAGS_INIT "/C 1252")
