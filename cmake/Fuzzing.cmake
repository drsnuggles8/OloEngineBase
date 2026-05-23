# Fuzzing.cmake
# Centralises the libFuzzer + ASan + UBSan configuration for OloEngine.
#
# When `OLO_ENABLE_FUZZING=ON` is passed at configure time, this module:
#
#   1. Globally defines `_DISABLE_VECTOR_ANNOTATION` / `_DISABLE_STRING_
#      ANNOTATION`. The MSVC STL flips its container ABI between
#      sanitised and non-sanitised TUs (the `annotate_string` /
#      `annotate_vector` flag); when only some targets are sanitised the
#      linker fires `/failifmismatch` errors. Forcing the flag off
#      uniformly keeps the STL ABI consistent across the whole build,
#      regardless of which targets we end up instrumenting.
#
#   2. Exposes two per-target helpers:
#        - `olo_apply_fuzz_sanitizer(TARGET)` — adds `-fsanitize=address,
#          undefined` instrumentation to the target, plus the runtime
#          libs (`clang_rt.asan_dynamic-x86_64.lib`, ubsan_standalone)
#          and `/wholearchive:asan_dynamic_runtime_thunk` link option.
#          Linkage is PUBLIC, so downstream consumers (OloEditor /
#          OloRuntime / fuzz harnesses) inherit the asan import lib at
#          link time without us having to instrument them too.
#        - `olo_apply_fuzzer_link(TARGET)` — for fuzz harnesses, on top
#          of `olo_apply_fuzz_sanitizer`: adds `-fsanitize=fuzzer`
#          coverage instrumentation and links `clang_rt.fuzzer-x86_64.lib`.
#
# Why per-target instead of `add_compile_options(-fsanitize=...)` globally:
# build-time tools (OloHeaderTool, protoc) need to *run* during the build,
# and instrumenting them means every invocation has to find the matching
# `clang_rt.asan_dynamic-x86_64.dll`. On GitHub-hosted runners that DLL
# name collides with MSVC's bundled ASan DLL on PATH, and Windows resolves
# the wrong one → STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139). Limiting
# instrumentation to OloEngine (the lib actually fuzzed through) and the
# harnesses themselves means tools stay clean and only the fuzz binaries
# need DLL discovery — which we handle in `.github/workflows/fuzz.yml`
# (and developers can replicate by prepending the runtime dir to PATH).
#
# On non-Windows (clang on Linux/macOS), CMake drives the link via clang,
# so the per-target helpers just pass `-fsanitize=...` and the right
# libclang_rt.* gets picked up automatically — no clang_rt path lookup,
# no PATH dance.
#
# This module is a no-op when `OLO_ENABLE_FUZZING=OFF` (the default).
# =============================================================================

option(OLO_ENABLE_FUZZING "Build libFuzzer harnesses (Clang only)" OFF)

if(NOT OLO_ENABLE_FUZZING)
    # Define the helpers as no-ops so callers don't have to gate every call.
    function(olo_apply_fuzz_sanitizer _target)
    endfunction()
    function(olo_apply_fuzzer_link _target)
    endfunction()
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "OloEngine fuzzing: OLO_ENABLE_FUZZING set but compiler is "
                   "'${CMAKE_CXX_COMPILER_ID}'. libFuzzer requires Clang. Skipping.")
    set(OLO_ENABLE_FUZZING OFF CACHE BOOL "" FORCE)
    function(olo_apply_fuzz_sanitizer _target)
    endfunction()
    function(olo_apply_fuzzer_link _target)
    endfunction()
    return()
endif()

message(STATUS "OloEngine fuzzing: enabled (Clang/libFuzzer)")

# Force-disable MSVC STL container annotations across the whole build so the
# `annotate_string` ABI flag stays the same value in every TU regardless of
# whether `-fsanitize=address` was passed. Without this, linking a sanitised
# OloEngine into a non-sanitised OloEditor (or vice versa) fails with
# `/failifmismatch: mismatch detected for 'annotate_string'`.
add_compile_definitions(_DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)

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
    if(NOT EXISTS "${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        message(FATAL_ERROR "OloEngine fuzzing: expected ASan runtime DLL at "
            "'${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.dll' but it "
            "isn't there. Install the LLVM compiler-rt component.")
    endif()
    message(STATUS "OloEngine fuzzing: clang runtime dir = ${_OLO_CLANG_RT_DIR}")

    # CI reads this file (`fuzz.yml`) to learn the exact directory CMake
    # resolved, then prepends it to $env:PATH so the fuzz harnesses load
    # the right ASan DLL. Critical because GitHub runners have multiple
    # LLVMs on PATH whose `clang_rt.asan_dynamic-x86_64.dll` files are
    # name-collision-but-ABI-mismatched against the .lib we link.
    file(WRITE "${CMAKE_BINARY_DIR}/olo_clang_rt_dir.txt"
         "${_OLO_CLANG_RT_DIR}\n")

    # Per-target sanitiser application. Linkage of the runtime libs uses
    # the plain `target_link_libraries(target ...)` signature (no PUBLIC /
    # PRIVATE keyword) because OloEngine's CMakeLists.txt uses plain calls
    # elsewhere, and CMake forbids mixing plain and keyword forms on the
    # same target. Plain-signature link libs propagate transitively to
    # consumers — equivalent to PUBLIC for our purposes — so downstream
    # consumers (OloEditor / OloRuntime / harnesses) inherit the asan
    # import lib at link time without needing to be instrumented
    # themselves; their .obj files have no __asan_* refs of their own,
    # the refs come from OloEngine's .obj files inside the static archive.
    function(olo_apply_fuzz_sanitizer _target)
        target_compile_options(${_target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_libraries(${_target}
            "${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.lib"
            "${_OLO_CLANG_RT_DIR}/clang_rt.ubsan_standalone-x86_64.lib"
            "${_OLO_CLANG_RT_DIR}/clang_rt.ubsan_standalone_cxx-x86_64.lib"
        )
        # target_link_options has no plain form — must use a keyword. INTERFACE
        # so the flags only attach to consumers' link line, not to OloEngine's
        # archive step (static libs aren't linked, so PRIVATE would do nothing
        # useful; PUBLIC would duplicate).
        target_link_options(${_target} INTERFACE
            "/wholearchive:${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
            "/INFERASANLIBS:NO"
        )
    endfunction()

    function(olo_apply_fuzzer_link _target)
        # Harnesses want full ASan/UBSan + libFuzzer coverage instrumentation
        # on their own translation unit too (it's a thin wrapper around the
        # engine's deserialiser, but if it has a bug we want ASan to find it).
        target_compile_options(${_target} PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
        )
        target_link_libraries(${_target} PRIVATE
            "${_OLO_CLANG_RT_DIR}/clang_rt.fuzzer-x86_64.lib"
        )
        # ASan/UBSan runtime libs come in via OloEngine's PUBLIC linkage
        # when the harness's `target_link_libraries(... OloEngine)` runs.
    endfunction()
else()
    # Linux/macOS: clang drives the link, so `-fsanitize=...` is honoured and
    # picks up the right libclang_rt.* automatically.
    function(olo_apply_fuzz_sanitizer _target)
        target_compile_options(${_target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        # INTERFACE: propagate to consumers only (static lib itself isn't linked).
        target_link_options(${_target} INTERFACE -fsanitize=address,undefined)
    endfunction()

    function(olo_apply_fuzzer_link _target)
        target_compile_options(${_target} PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
        )
        target_link_options(${_target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endfunction()
endif()
