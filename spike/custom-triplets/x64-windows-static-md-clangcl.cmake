# OloEngine custom triplet (issue #773/#774 vcpkg spike).
#
# Same static-lib / dynamic-CRT linkage as x64-windows-static-md.cmake, but
# chainloads OUR clang-cl toolchain file so vcpkg builds every port with the
# exact same compiler (and the same clang-cl gotcha fixes — llvm-rc, the
# unused-command-line-argument suppression) the engine itself builds with
# under the `clangcl` CMake preset (Ninja Multi-Config). This is the specific
# unknown #774 exists to de-risk: does a clang-cl chainloaded triplet work at
# all under Ninja Multi-Config manifest mode.
#
# A different compiler is a different ABI, so this MUST be a distinct triplet
# name from x64-windows-static-md — vcpkg's binary-cache key includes the
# triplet name, so mixing cl.exe-built and clang-cl-built archives under one
# triplet would silently serve the wrong ABI from the cache.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../../cmake/ClangCLToolchain.cmake")

# GOTCHA #1 (found by this spike): vcpkg builds each port in a curated,
# sanitized environment — it does NOT pass the invoking shell's PATH through
# to port builds by default. Our production ClangCLToolchain.cmake sets
# CMAKE_C(XX)_COMPILER to the bare name "clang-cl" and relies on PATH
# resolution (which is fine for the engine's own interactive CMake configure,
# where PATH is intact) — under vcpkg that bare name resolves to nothing and
# every port fails at `enable_language()` with "is not a full path and was
# not found in the PATH." Passing PATH through is required whenever a
# chainloaded toolchain resolves its compiler by name rather than absolute
# path.
#
# GOTCHA #2 (found by this spike, second-order): plain VCPKG_ENV_PASSTHROUGH
# folds the passed-through variable's VALUE into the port's ABI hash. PATH
# strings routinely differ byte-for-byte across worktrees/shells/CI runners
# even when they resolve to the same clang-cl.exe (different ordering, an
# extra prepended entry, different drive-letter casing) — which would silently
# defeat the entire point of #773 (a cache hit that depends on an identical
# PATH string is not a reliable cross-worktree cache hit). Use the
# "_UNTRACKED" form instead: it makes the variable visible to the build without
# hashing its value, so the compiler still resolves by name but the cache key
# stays stable across machines/shells with differently-ordered-but-equivalent
# PATH values.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED PATH)
