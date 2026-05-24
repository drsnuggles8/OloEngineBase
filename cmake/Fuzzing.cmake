# Fuzzing.cmake
# Centralises the libFuzzer + ASan + UBSan configuration for OloEngine.
#
# When `OLO_ENABLE_FUZZING=ON` is passed at configure time, this module:
#
#   1. Globally defines `_DISABLE_VECTOR_ANNOTATION` / `_DISABLE_STRING_
#      ANNOTATION`. The MSVC STL flips its container ABI between
#      sanitised and non-sanitised TUs (`annotate_string` /
#      `annotate_vector`); when only some targets are sanitised the
#      linker fires `/failifmismatch` errors. Forcing the flag off
#      uniformly keeps the STL ABI consistent across the whole build.
#
#   2. Builds libFuzzer from compiler-rt source (LLVM 19.1.7 release
#      tarball) into a static `olo_libfuzzer` target. We can't use stock
#      `clang_rt.fuzzer-x86_64.lib` because LLVM only ships it built /MT,
#      while the Vulkan SDK's prebuilt spirv-cross / shaderc are /MD —
#      switching the whole build to /MT to match libFuzzer would force
#      us to rebuild those vendor libs too. Building libFuzzer ourselves
#      with the project's /MD CRT is the smallest deviation.
#
#   3. Exposes two per-target helpers:
#        - `olo_apply_fuzz_sanitizer(TARGET)` — adds `-fsanitize=address,
#          undefined` instrumentation to the target, plus the runtime
#          libs (`clang_rt.asan_dynamic-x86_64.lib`, ubsan_standalone)
#          and `/wholearchive:asan_dynamic_runtime_thunk` link option.
#          Linkage of runtime libs uses the plain `target_link_libraries`
#          signature so OloEngine's CMakeLists doesn't refuse to mix
#          keyword + plain forms; that propagates to consumers
#          (OloEditor / OloRuntime / fuzz harnesses) so their link line
#          gets the import lib without needing the consumer to be
#          instrumented.
#        - `olo_apply_fuzzer_link(TARGET)` — for fuzz harnesses, on top
#          of `olo_apply_fuzz_sanitizer`: adds `-fsanitize=fuzzer`
#          coverage instrumentation and links our `olo_libfuzzer`.
#
# Why per-target instead of `add_compile_options(-fsanitize=...)` globally:
# build-time tools (OloHeaderTool, protoc) run during the build; if they
# were instrumented they'd also need `clang_rt.asan_dynamic-x86_64.dll`
# discoverable at run time. On GitHub-hosted runners that DLL name
# collides with MSVC's bundled ASan DLL on PATH (different exports →
# STATUS_ENTRYPOINT_NOT_FOUND, 0xC0000139). Limiting instrumentation to
# OloEngine and the harnesses keeps tools clean. Fuzz harnesses still
# need the DLL at run time; `.github/workflows/fuzz.yml` prepends the
# clang_rt dir to PATH in the run step, and devs can do the same locally.
#
# On non-Windows (clang on Linux/macOS), CMake drives the link via clang,
# so the per-target helpers just pass `-fsanitize=...` and the right
# libclang_rt.* gets picked up automatically — no clang_rt path lookup,
# no PATH dance, no from-source libFuzzer build.
#
# This module is a no-op when `OLO_ENABLE_FUZZING=OFF` (the default).
# =============================================================================

option(OLO_ENABLE_FUZZING "Build libFuzzer harnesses (Clang only)" OFF)

# Which sanitiser the fuzz harnesses link against. We can't link ASan and
# UBSan together on Windows: compiler-rt ships
# `sanitizer_coverage_win_sections.cpp.obj` (defining the `__start___sancov_*`
# / `__stop___sancov_*` section markers) inside BOTH
# `clang_rt.asan_dynamic_runtime_thunk-x86_64.lib` AND
# `clang_rt.ubsan_standalone-x86_64.lib`, so a combined link line trips
# lld-link duplicate-symbol errors (LLVM 19 and LLVM 20 both affected).
# CI runs the harnesses twice — once with `asan` and once with `ubsan`
# (matrix in fuzz.yml) — so each link line only references one of the libs.
set(OLO_FUZZ_SANITIZER "asan" CACHE STRING "Which sanitiser fuzz harnesses link against (asan|ubsan)")
set_property(CACHE OLO_FUZZ_SANITIZER PROPERTY STRINGS asan ubsan)

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

if(NOT OLO_FUZZ_SANITIZER MATCHES "^(asan|ubsan)$")
    message(FATAL_ERROR "OloEngine fuzzing: OLO_FUZZ_SANITIZER must be 'asan' or "
        "'ubsan', got '${OLO_FUZZ_SANITIZER}'.")
endif()

message(STATUS "OloEngine fuzzing: enabled (Clang/libFuzzer + ${OLO_FUZZ_SANITIZER})")

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

    # Locate the directory containing the ASan runtime libs. clang-cl
    # installations can use either of two layouts on Windows:
    #   - new per-target:  <prefix>/lib/clang/<ver>/lib/<triple>/
    #     (reported by `clang-cl -print-runtime-dir`, LLVM ≥ 16)
    #   - legacy per-arch: <prefix>/lib/clang/<ver>/lib/windows/
    #     (where LLVM's Windows installer / VS2022-bundled clang-cl puts
    #     them through at least LLVM 21)
    # find_path walks both candidates so we don't hardcode either.
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-runtime-dir
        OUTPUT_VARIABLE _OLO_CLANG_RT_DIR_HINT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(_OLO_CLANG_RT_LEGACY_HINT "${_OLO_CLANG_RT_DIR_HINT}/../windows")

    find_path(_OLO_CLANG_RT_DIR
        NAMES clang_rt.asan_dynamic-x86_64.lib
        HINTS "${_OLO_CLANG_RT_DIR_HINT}" "${_OLO_CLANG_RT_LEGACY_HINT}"
        NO_DEFAULT_PATH
    )
    if(NOT _OLO_CLANG_RT_DIR)
        message(FATAL_ERROR "OloEngine fuzzing: failed to locate "
            "clang_rt.asan_dynamic-x86_64.lib near `${_OLO_CLANG_RT_DIR_HINT}`. "
            "Install the Clang compiler-rt libraries (`compiler-rt` component "
            "in the LLVM installer / `Clang-cl runtime` in Visual Studio).")
    endif()
    if(NOT EXISTS "${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        message(FATAL_ERROR "OloEngine fuzzing: expected ASan runtime DLL at "
            "'${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.dll' but it "
            "isn't there. Install the LLVM compiler-rt component.")
    endif()
    message(STATUS "OloEngine fuzzing: clang runtime dir = ${_OLO_CLANG_RT_DIR}")

    # CI's "Run fuzz smoke" step reads this file to learn the exact dir
    # CMake resolved, then prepends it to $env:PATH so the harnesses load
    # the matching ASan DLL at run time. Critical because GitHub runners
    # have multiple LLVMs on PATH whose `clang_rt.asan_dynamic-x86_64.dll`
    # files are name-collision-but-ABI-mismatched.
    file(WRITE "${CMAKE_BINARY_DIR}/olo_clang_rt_dir.txt"
         "${_OLO_CLANG_RT_DIR}\n")

    # --- Build libFuzzer from compiler-rt source ---
    # LLVM's stock `clang_rt.fuzzer-x86_64.lib` is built /MT; our build is
    # /MD (to match Vulkan SDK prebuilts) so the prebuilt lib trips
    # /failifmismatch: RuntimeLibrary. We compile the fuzzer sources
    # ourselves with our CRT settings to dodge that.
    #
    # Use file(DOWNLOAD) + file(ARCHIVE_EXTRACT) instead of FetchContent —
    # compiler-rt's own CMakeLists.txt has dependencies on LLVM helper
    # macros we don't have (e.g. extend_path from base-config-ix.cmake),
    # so FetchContent_MakeAvailable would try to add_subdirectory and
    # error immediately.
    # Normalise FETCHCONTENT_BASE_DIR — CI passes it with native Windows
    # backslashes (`D:\a\…`) and CMake later trips on the `\a` escape when
    # the path appears inside an `add_library(... ${src})` source list.
    file(TO_CMAKE_PATH "${FETCHCONTENT_BASE_DIR}" _OLO_FETCH_BASE)
    set(_OLO_FUZZER_DIR "${_OLO_FETCH_BASE}/compiler-rt-19.1.7.src")
    set(_OLO_FUZZER_TARBALL "${_OLO_FETCH_BASE}/compiler-rt-19.1.7.src.tar.xz")
    if(NOT EXISTS "${_OLO_FUZZER_DIR}/lib/fuzzer/FuzzerLoop.cpp")
        message(STATUS "OloEngine fuzzing: downloading compiler-rt 19.1.7 source")
        file(DOWNLOAD
            https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/compiler-rt-19.1.7.src.tar.xz
            "${_OLO_FUZZER_TARBALL}"
            EXPECTED_HASH SHA256=c12b6e764202c615c1a3af9a13d477846878757ae0e29e5f8979215a6958fffc
            SHOW_PROGRESS
        )
        file(ARCHIVE_EXTRACT
            INPUT "${_OLO_FUZZER_TARBALL}"
            DESTINATION "${FETCHCONTENT_BASE_DIR}"
        )
    endif()
    set(_OLO_FUZZER_SRC "${_OLO_FUZZER_DIR}/lib/fuzzer")

    # Windows-only source list (FuzzerIOPosix.cpp, FuzzerUtilLinux.cpp etc.
    # only compile on their respective platforms).
    set(_OLO_FUZZER_SOURCES
        ${_OLO_FUZZER_SRC}/FuzzerCrossOver.cpp
        ${_OLO_FUZZER_SRC}/FuzzerDataFlowTrace.cpp
        ${_OLO_FUZZER_SRC}/FuzzerDriver.cpp
        ${_OLO_FUZZER_SRC}/FuzzerExtFunctionsWindows.cpp
        ${_OLO_FUZZER_SRC}/FuzzerExtraCounters.cpp
        ${_OLO_FUZZER_SRC}/FuzzerExtraCountersWindows.cpp
        ${_OLO_FUZZER_SRC}/FuzzerFork.cpp
        ${_OLO_FUZZER_SRC}/FuzzerIO.cpp
        ${_OLO_FUZZER_SRC}/FuzzerIOWindows.cpp
        ${_OLO_FUZZER_SRC}/FuzzerLoop.cpp
        ${_OLO_FUZZER_SRC}/FuzzerMain.cpp
        ${_OLO_FUZZER_SRC}/FuzzerMerge.cpp
        ${_OLO_FUZZER_SRC}/FuzzerMutate.cpp
        ${_OLO_FUZZER_SRC}/FuzzerSHA1.cpp
        ${_OLO_FUZZER_SRC}/FuzzerTracePC.cpp
        ${_OLO_FUZZER_SRC}/FuzzerUtil.cpp
        ${_OLO_FUZZER_SRC}/FuzzerUtilWindows.cpp
    )

    add_library(olo_libfuzzer STATIC ${_OLO_FUZZER_SOURCES})
    target_include_directories(olo_libfuzzer PRIVATE ${_OLO_FUZZER_SRC})
    # libFuzzer's own code is C++17; the engine is C++23 but that doesn't
    # matter for this isolated static lib.
    target_compile_features(olo_libfuzzer PRIVATE cxx_std_17)
    # NEVER instrument libFuzzer with -fsanitize=fuzzer — that would
    # recursively coverage-trace the fuzzer itself. We also skip ASan/UBSan
    # on libFuzzer so its allocations don't get tracked (the fuzzer needs
    # to own the input buffer without ASan poisoning).
    target_compile_definitions(olo_libfuzzer PRIVATE
        # libFuzzer expects to detect ASan at runtime via __asan_default_options;
        # nothing for us to define here.
    )
    set_target_properties(olo_libfuzzer PROPERTIES
        # Inherit the project's /MD CRT (set above) so the .obj files have
        # `MD_DynamicRelease` and match the rest of the link.
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        FOLDER "Test/Fuzzing"
    )

    # Per-target sanitiser application. Linkage of the runtime libs uses
    # the plain `target_link_libraries(target ...)` signature (no PUBLIC /
    # PRIVATE keyword) because OloEngine's CMakeLists.txt uses plain calls
    # elsewhere, and CMake forbids mixing plain and keyword forms on the
    # same target. Plain-signature link libs propagate transitively to
    # consumers.
    #
    # Branches on `OLO_FUZZ_SANITIZER`: the per-build choice is ASan OR
    # UBSan, never both — see the header note on the
    # `sanitizer_coverage_win_sections.cpp.obj` duplicate-symbol issue.
    if(OLO_FUZZ_SANITIZER STREQUAL "asan")
        function(olo_apply_fuzz_sanitizer _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=address -fno-omit-frame-pointer
            )
            target_link_libraries(${_target}
                "${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic-x86_64.lib"
            )
            # target_link_options has no plain form — must use a keyword.
            # INTERFACE so the flags only attach to consumers' link line, not
            # to OloEngine's archive step (static libs aren't linked, so
            # PRIVATE would do nothing useful; PUBLIC would duplicate at the
            # lib step).
            target_link_options(${_target} INTERFACE
                "/wholearchive:${_OLO_CLANG_RT_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
                "/INFERASANLIBS:NO"
            )
        endfunction()

        function(olo_apply_fuzzer_link _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=fuzzer,address -fno-omit-frame-pointer
            )
            target_link_libraries(${_target} PRIVATE olo_libfuzzer)
        endfunction()
    else() # ubsan
        # UBSan on Windows is statically linked — no DLL deployment dance.
        # `-fno-sanitize-recover=undefined` makes every check fatal so
        # libFuzzer's death callback fires on a real UB hit, otherwise UBSan
        # would log and continue and the fuzzer would never see the crash.
        function(olo_apply_fuzz_sanitizer _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=undefined -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer
            )
            target_link_libraries(${_target}
                "${_OLO_CLANG_RT_DIR}/clang_rt.ubsan_standalone-x86_64.lib"
                "${_OLO_CLANG_RT_DIR}/clang_rt.ubsan_standalone_cxx-x86_64.lib"
            )
        endfunction()

        function(olo_apply_fuzzer_link _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=fuzzer,undefined -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer
            )
            target_link_libraries(${_target} PRIVATE olo_libfuzzer)
        endfunction()
    endif()
else()
    # Linux/macOS: clang drives the link, so `-fsanitize=...` is honoured and
    # picks up the right libclang_rt.* automatically. No duplicate-symbol
    # mess (the Windows packaging bug doesn't apply), so OLO_FUZZ_SANITIZER
    # just selects which compile/link flag we pass.
    if(OLO_FUZZ_SANITIZER STREQUAL "asan")
        function(olo_apply_fuzz_sanitizer _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=address -fno-omit-frame-pointer
            )
            target_link_options(${_target} INTERFACE -fsanitize=address)
        endfunction()
        function(olo_apply_fuzzer_link _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=fuzzer,address -fno-omit-frame-pointer
            )
            target_link_options(${_target} PRIVATE -fsanitize=fuzzer,address)
        endfunction()
    else() # ubsan
        function(olo_apply_fuzz_sanitizer _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=undefined -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer
            )
            target_link_options(${_target} INTERFACE -fsanitize=undefined)
        endfunction()
        function(olo_apply_fuzzer_link _target)
            target_compile_options(${_target} PRIVATE
                -fsanitize=fuzzer,undefined -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer
            )
            target_link_options(${_target} PRIVATE -fsanitize=fuzzer,undefined)
        endfunction()
    endif()
endif()
