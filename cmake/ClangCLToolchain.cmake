# ClangCLToolchain.cmake
# Toolchain file for building with clang-cl (Clang with MSVC ABI) via Ninja.
# Used by the "clangcl" CMake preset.

set(CMAKE_C_COMPILER "clang-cl" CACHE STRING "")
set(CMAKE_CXX_COMPILER "clang-cl" CACHE STRING "")
set(CMAKE_LINKER "lld-link" CACHE STRING "")

# Use MSVC's rc.exe for resource compilation — llvm-rc can't handle
# non-ASCII characters in vendor .rc files (e.g. assimp's copyright symbol).
# NOTE: Update the SDK version if your Windows SDK version differs.
find_program(_RC_COMPILER rc
    PATHS "C:/Program Files (x86)/Windows Kits/10/bin"
    PATH_SUFFIXES "10.0.26100.0/x64" "10.0.22621.0/x64" "10.0.22000.0/x64"
    NO_DEFAULT_PATH
)
if(_RC_COMPILER)
    set(CMAKE_RC_COMPILER "${_RC_COMPILER}" CACHE FILEPATH "")
endif()

# Prevent ccache from wrapping cmake_llvm_rc — ccache doesn't understand
# the cmake -E cmake_llvm_rc utility invocation used for RC compilation.
set(CMAKE_RC_COMPILER_LAUNCHER "" CACHE STRING "")

# Suppress errors for MSVC-only flags (e.g. /experimental:c11atomics) that
# vendor libraries pass but clang-cl doesn't recognize.
set(CMAKE_C_FLAGS_INIT "-Wno-error=unused-command-line-argument")
set(CMAKE_CXX_FLAGS_INIT "-Wno-error=unused-command-line-argument")
