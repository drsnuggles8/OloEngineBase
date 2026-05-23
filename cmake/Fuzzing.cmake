# Fuzzing.cmake
# Centralises the libFuzzer + ASan + UBSan configuration for OloEngine.
#
# When `OLO_ENABLE_FUZZING=ON` is passed at configure time, this module:
#
#   1. Propagates `-fsanitize=address,undefined` to **every** target in the
#      build via `add_compile_options`. The fuzz harnesses link against the
#      OloEngine static lib; if only the harness is sanitised, the STL
#      `annotate_string` / `annotate_vector` ABI flips between TUs and lld-link
#      fires `/failifmismatch` errors. Propagating the flag globally keeps the
#      ABI consistent.
#
#   2. Exposes `olo_apply_fuzzer_link(TARGET)` for the per-harness link step.
#      On Windows + clang-cl the standard pattern `target_link_options(target
#      PRIVATE -fsanitize=fuzzer,address,undefined)` is **broken**: CMake
#      invokes `lld-link` directly (not via the clang-cl driver), so the
#      `-fsanitize=` flags are passed straight to the linker, which doesn't
#      know what they mean and silently ignores them. Result: undefined
#      `__asan_*` / `__sanitizer_cov_*` symbols at link time. Microsoft's
#      documented workaround is to link the `clang_rt.*.lib` import libs
#      explicitly and disable the auto-ASan-libs inference; this function
#      does exactly that.
#
#      On non-Windows (clang on Linux/macOS), the standard `-fsanitize=...`
#      link flag works because CMake drives the link through `clang`.
#
# This module is a no-op when `OLO_ENABLE_FUZZING=OFF` (the default).
# =============================================================================

option(OLO_ENABLE_FUZZING "Build libFuzzer harnesses (Clang only)" OFF)

if(NOT OLO_ENABLE_FUZZING)
    # Define the helper as a no-op so callers don't have to gate every call.
    function(olo_apply_fuzzer_link _target)
    endfunction()
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "OloEngine fuzzing: OLO_ENABLE_FUZZING set but compiler is "
                   "'${CMAKE_CXX_COMPILER_ID}'. libFuzzer requires Clang. Skipping.")
    set(OLO_ENABLE_FUZZING OFF CACHE BOOL "" FORCE)
    function(olo_apply_fuzzer_link _target)
    endfunction()
    return()
endif()

message(STATUS "OloEngine fuzzing: enabled (Clang/libFuzzer)")

# --- Global compile flags ---
# Every TU in the build gets ASan + UBSan instrumentation so the STL annotation
# ABI matches between the OloEngine static lib and the per-harness binary.
# libFuzzer instrumentation (`-fsanitize=fuzzer`) is added only to the harness
# itself in `olo_apply_fuzzer_link` (see below) — we don't want libFuzzer
# coverage hooks in the OloEditor / OloRuntime / tests binaries.
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)

if(WIN32)
    # MSVC ASan runtime requires the release dynamic CRT (/MD). Mixing with
    # /MDd (debug CRT) causes `_ITERATOR_DEBUG_LEVEL` mismatches against the
    # shipped clang_rt.asan_dynamic-x86_64.lib, which is built /MD-only.
    # Matches the pattern Sanitizers.cmake uses for OLO_ENABLE_ASAN.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

    # ASan is incompatible with /RTC1 and /ZI (Edit-and-Continue). Strip the
    # offenders out of the default Debug flags. Fuzzing.cmake is include()-d
    # (not added as a subdirectory) so we mutate the variables in the
    # top-level scope directly — no PARENT_SCOPE plumbing needed.
    foreach(_flag IN ITEMS CMAKE_CXX_FLAGS_DEBUG CMAKE_C_FLAGS_DEBUG)
        string(REPLACE "/RTC1" "" ${_flag} "${${_flag}}")
        string(REPLACE "/RTCs" "" ${_flag} "${${_flag}}")
        string(REPLACE "/RTCu" "" ${_flag} "${${_flag}}")
        string(REPLACE "/ZI"   "/Zi" ${_flag} "${${_flag}}")
    endforeach()
    add_link_options(/INCREMENTAL:NO)

    # Locate the directory containing `clang_rt.fuzzer-x86_64.lib`. The clang
    # installation can use either of two layouts on Windows:
    #   - new per-target:  <prefix>/lib/clang/<ver>/lib/<triple>/
    #     (reported by `clang-cl -print-runtime-dir`, LLVM ≥ 16)
    #   - legacy per-arch: <prefix>/lib/clang/<ver>/lib/windows/
    #     (still where LLVM's Windows installer / VS-bundled clang-cl puts
    #     the .lib files as of LLVM 21)
    # `find_path` walks both candidates and reports whichever wins, so we
    # don't have to hardcode either.
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-runtime-dir
        OUTPUT_VARIABLE _OLO_CLANG_RT_DIR_HINT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(_OLO_CLANG_RT_LEGACY_HINT "${_OLO_CLANG_RT_DIR_HINT}/../windows")

    find_path(_OLO_CLANG_RT_DIR
        NAMES clang_rt.fuzzer-x86_64.lib
        HINTS "${_OLO_CLANG_RT_DIR_HINT}" "${_OLO_CLANG_RT_LEGACY_HINT}"
        NO_DEFAULT_PATH
    )
    if(NOT _OLO_CLANG_RT_DIR)
        message(FATAL_ERROR "OloEngine fuzzing: failed to locate "
            "clang_rt.fuzzer-x86_64.lib near `${_OLO_CLANG_RT_DIR_HINT}`. "
            "Install the Clang compiler-rt libraries (`compiler-rt` component "
            "in the LLVM installer / `Clang-cl runtime` in Visual Studio).")
    endif()
    message(STATUS "OloEngine fuzzing: clang runtime dir = ${_OLO_CLANG_RT_DIR}")

    # Required runtime libs for ASan (dynamic) + UBSan on Windows ClangCL.
    # Names match LLVM's compiler-rt output on Windows
    # (clang_rt.<sanitizer>[-<arch>].lib). These need to link into EVERY
    # final binary because the global -fsanitize=address,undefined compile
    # flag instruments every TU — without the runtime, every link step
    # (OloHeaderTool, protoc, OloEditor, fuzz harnesses, …) reports
    # undefined __asan_* / __ubsan_* symbols.
    set(_OLO_SAN_RT_LIBS
        "${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.lib"
        "${_OLO_CLANG_RT_DIR}/clang_rt.ubsan_standalone-x86_64.lib"
        "${_OLO_CLANG_RT_DIR}/clang_rt.ubsan_standalone_cxx-x86_64.lib"
    )
    # The asan_dynamic_runtime_thunk must be linked with /wholearchive so its
    # interceptors register at load time. lld-link's MSVC-compatible flag.
    set(_OLO_SAN_LINK_FLAGS
        "/wholearchive:${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
        # Skip MSVC's auto-inferred ASan libs (incompatible with clang_rt).
        "/INFERASANLIBS:NO"
    )
    # Stash the libFuzzer lib for the per-harness step (not in the global
    # link set — non-fuzz binaries don't need the fuzzer driver).
    set(_OLO_FUZZER_RT_LIB
        "${_OLO_CLANG_RT_DIR}/clang_rt.fuzzer-x86_64.lib"
        CACHE INTERNAL "libFuzzer runtime lib (Windows ClangCL)"
    )

    # Apply globally — every executable/DLL link picks these up. Static
    # libs ignore link_libraries() inputs in terms of linkage but record
    # them as INTERFACE deps that propagate to downstream consumers.
    link_libraries(${_OLO_SAN_RT_LIBS})
    add_link_options(${_OLO_SAN_LINK_FLAGS})

    function(olo_apply_fuzzer_link _target)
        # libFuzzer instrumentation is per-harness (NOT propagated globally —
        # we don't want the fuzzer coverage hooks in non-fuzz binaries).
        target_compile_options(${_target} PRIVATE -fsanitize=fuzzer)
        target_link_libraries(${_target} PRIVATE ${_OLO_FUZZER_RT_LIB})
    endfunction()
else()
    # Linux/macOS: clang drives the link, so `-fsanitize=...` is honoured and
    # picks up the right libclang_rt.* automatically.
    add_link_options(-fsanitize=address,undefined)

    function(olo_apply_fuzzer_link _target)
        target_compile_options(${_target} PRIVATE -fsanitize=fuzzer)
        target_link_options(${_target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endfunction()
endif()
