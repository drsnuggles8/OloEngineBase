# OloEngine vcpkg triplet — clang-cl (Clang with the MSVC ABI), static libraries
# against the DYNAMIC CRT. Same linkage as x64-windows-static-md.cmake (read the
# CRT rationale there); the difference is that this one chainloads OUR clang-cl
# toolchain file, so vcpkg builds every port with the exact compiler, linker and
# resource-compiler settings the `clangcl` preset (Ninja Multi-Config) uses for
# the engine itself.
#
# A different compiler is a different ABI, so this MUST stay a distinct triplet
# NAME from x64-windows-static-md — vcpkg's binary-cache key includes the triplet
# name, so mixing cl.exe-built and clang-cl-built archives under one triplet
# would silently serve the wrong ABI out of the cache.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../ClangCLToolchain.cmake")

# PER-PORT EXCEPTION: libsodium builds with the stock MSVC toolchain even here.
#
# Its portfile picks between two build systems by sniffing the detected C compiler:
# cl.exe takes the bundled MSBuild solution, anything else takes an autotools path
# (the portfile's own comment: "the msbuild solution only builds with MSVC; other
# compilers targeting windows ... go through the make-based path"). Under a
# chainloaded clang-cl that autotools path fails at `configure`:
#
#   configure: error: C compiler cannot create executables
#
# because vcpkg-make hands libtool's `compile` wrapper a mangled command line
# (CC='compile clang-cl.exe', CFLAGS full of '-Xcompiler -Zi -Xcompiler -Ob0 …',
# LDFLAGS with tripled '-Xlinker'). That is a vcpkg-make/clang-cl integration
# problem, not something this repo should carry a forked portfile for.
#
# Clearing the chainload for this one port makes vcpkg detect cl.exe and take the
# MSBuild path, so libsodium ends up MSVC-built inside an otherwise clang-cl
# triplet. That is safe here and only here: libsodium is a pure C library consumed
# across the platform C ABI, with the same dynamic CRT on both sides, so there is
# no C++ ABI surface to mismatch. Do NOT extend this to a C++ port without a much
# harder look. `PORT` is set by vcpkg when it loads the triplet for each port, and
# the triplet's contents feed the ABI hash, so this stays reproducible and cached.
if(PORT STREQUAL "libsodium")
    unset(VCPKG_CHAINLOAD_TOOLCHAIN_FILE)
endif()

# GOTCHA #1 (found by the #774 spike): vcpkg builds each port in a curated,
# sanitized environment — it does NOT pass the invoking shell's PATH through to
# port builds by default. ClangCLToolchain.cmake sets CMAKE_C(XX)_COMPILER to the
# bare name "clang-cl" and relies on PATH resolution (fine for the engine's own
# interactive CMake configure, where PATH is intact) — under vcpkg that bare name
# resolves to nothing and every port fails at `enable_language()` with "is not a
# full path and was not found in the PATH." Passing PATH through is required
# whenever a chainloaded toolchain resolves its compiler by name rather than by
# absolute path.
#
# GOTCHA #2 (the dangerous one): plain VCPKG_ENV_PASSTHROUGH folds the passed-
# through variable's VALUE into the port's ABI hash. PATH strings routinely
# differ byte-for-byte across worktrees/shells/CI runners even when they resolve
# to the same clang-cl.exe (different ordering, an extra prepended entry,
# different drive-letter casing) — which would silently defeat the entire point
# of issue #773 (a cache hit that depends on an identical PATH string is not a
# reliable cross-worktree cache hit). The "_UNTRACKED" form makes the variable
# visible to the build WITHOUT hashing its value, so the compiler still resolves
# by name but the cache key stays stable. This fails silently if you get it
# wrong: builds still succeed, they are just orders of magnitude slower than a
# cache restore, and nothing in the log flags it.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED PATH)
