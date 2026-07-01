# Sanitizers.cmake
# Optional sanitizer support for detecting memory errors at runtime.
#
# Available sanitizers and platform support:
#
#   Option                | MSVC    | GCC/Clang  | Detects
#   ----------------------|---------|------------|----------------------------------------
#   OLO_ENABLE_ASAN       | Yes     | Yes        | Use-after-free, buffer overflow, stack overflow
#   OLO_ENABLE_LSAN       | No      | Yes (Linux)| Memory leaks (standalone or with ASan)
#   OLO_ENABLE_UBSAN      | No      | Yes        | Undefined behavior (signed overflow, null deref, etc.)
#   OLO_ENABLE_TSAN       | No      | Yes        | Data races (incompatible with ASan)
#
# Usage:
#   cmake -B build -DOLO_ENABLE_ASAN=ON                    # ASan only
#   cmake -B build -DOLO_ENABLE_ASAN=ON -DOLO_ENABLE_UBSAN=ON  # ASan + UBSan (combinable)
#   cmake -B build -DOLO_ENABLE_TSAN=ON                    # TSan (alone, not with ASan)
#
# Notes:
#   - MSVC ASan is incompatible with /RTC1 and /ZI (Edit-and-Continue).
#   - TSan and ASan cannot be used together.
#   - UBSan can be combined with ASan or used alone.
#   - LSan is enabled automatically with ASan on Linux. Use standalone for leak-only checks.

if(DEFINED OLO_SANITIZERS_INCLUDED)
    return()
endif()
set(OLO_SANITIZERS_INCLUDED TRUE)

option(OLO_ENABLE_ASAN  "Enable Address Sanitizer (ASan)" OFF)
option(OLO_ENABLE_LSAN  "Enable Leak Sanitizer (LSan) — Linux GCC/Clang only" OFF)
option(OLO_ENABLE_UBSAN "Enable Undefined Behavior Sanitizer (UBSan) — GCC/Clang only" OFF)
option(OLO_ENABLE_TSAN  "Enable Thread Sanitizer (TSan) — GCC/Clang only, incompatible with ASan" OFF)

# Validate incompatible combinations
if(OLO_ENABLE_TSAN AND OLO_ENABLE_ASAN)
    message(FATAL_ERROR "TSan and ASan cannot be used together. Disable one of them.")
endif()

# Tracy + TSan don't compose: Tracy's TracyClient.cpp has file-scope globals
# (Profiler, _memory_span_cache) whose static-init order races between the
# Profiler ctor (which spawns the sampling thread) and the rpmalloc global
# cache. TSan correctly flags it and halts the binary, which trips
# gtest_discover_tests at build time. Force Tracy off whenever TSan is on, so
# direct `cmake -DOLO_ENABLE_TSAN=ON …` invocations get the right behaviour
# without needing the matching `-DTRACY_ENABLE=OFF`. Setting the cache var
# here (before OloEngine/vendor/CMakeLists.txt declares the option) ensures
# the option() call leaves the cache value alone.
if(OLO_ENABLE_TSAN)
    set(TRACY_ENABLE OFF CACHE BOOL "Disabled automatically because OLO_ENABLE_TSAN=ON (Tracy's static-init races with TSan)." FORCE)
endif()

# --- Address Sanitizer ---
if(OLO_ENABLE_ASAN)
    message(STATUS "Address Sanitizer (ASan) enabled")

    if(MSVC)
        add_compile_options(/fsanitize=address)

        # Force release dynamic CRT (/MD) for all configurations when ASan is active.
        # MSVC ASan runtime links against release CRT; mixing with /MDd causes
        # _ITERATOR_DEBUG_LEVEL mismatches against release-only third-party static libs
        # (e.g. Vulkan SDK's spirv-cross on CI where debug libs aren't available).
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

        # ASan is incompatible with /RTC1 (runtime checks) and incremental linking.
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTCs" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTCu" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        string(REPLACE "/RTCs" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        string(REPLACE "/RTCu" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        # ASan is also incompatible with Edit-and-Continue (/ZI).
        string(REPLACE "/ZI" "/Zi" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/ZI" "/Zi" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        add_link_options(/INCREMENTAL:NO)

        # clang-cl reports as MSVC, so it takes this branch. But unlike cl.exe — whose
        # `/fsanitize=address` embeds `/defaultlib:clang_rt.asan*` directives that
        # link.exe auto-resolves from the `LIB` env var — clang-cl neither embeds those
        # directives nor gets its compiler-rt runtime dir onto the search path when CMake
        # drives the link with lld-link directly (the MSVC linker model). So every
        # `__asan_*` symbol comes up undefined (first surfaces linking the vendored
        # protobuf `protoc.exe`). Fix both halves below: locate clang's runtime dir and
        # name the runtime libs explicitly, exactly as the clang driver would.
        #
        # Resolving that dir is version-dependent: `-print-runtime-dir` is the intended
        # query, but newer LLVM (e.g. 21) reports the per-target layout
        # (lib/<triple>) that doesn't exist on installs still shipping the legacy
        # lib/windows layout, while older LLVM reports lib/windows directly. Probe both
        # (runtime-dir, then resource-dir/lib/windows) and pick whichever actually holds
        # the ASan import lib, so it works regardless of the LLVM version on PATH.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(_olo_asan_import_lib "clang_rt.asan_dynamic-x86_64.lib")
            set(_olo_asan_candidates "")

            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" -print-runtime-dir
                OUTPUT_VARIABLE _olo_rt_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_olo_rt_dir)
                file(TO_CMAKE_PATH "${_olo_rt_dir}" _olo_rt_dir)
                list(APPEND _olo_asan_candidates "${_olo_rt_dir}")
            endif()

            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
                OUTPUT_VARIABLE _olo_res_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_olo_res_dir)
                file(TO_CMAKE_PATH "${_olo_res_dir}" _olo_res_dir)
                list(APPEND _olo_asan_candidates "${_olo_res_dir}/lib/windows")
            endif()

            set(_olo_asan_runtime_dir "")
            foreach(_olo_cand IN LISTS _olo_asan_candidates)
                if(EXISTS "${_olo_cand}/${_olo_asan_import_lib}")
                    set(_olo_asan_runtime_dir "${_olo_cand}")
                    break()
                endif()
            endforeach()

            if(_olo_asan_runtime_dir)
                # Unlike cl.exe, clang-cl does NOT embed a `/defaultlib:clang_rt.asan*`
                # directive in its objects, so putting the dir on the search path isn't
                # enough — the runtime must be named explicitly, exactly as the clang
                # driver does when IT links: the dynamic ASan import lib, plus the
                # per-module thunk force-included with /wholearchive (it supplies module
                # globals like __asan_shadow_memory_dynamic_address).
                add_link_options(
                    "/LIBPATH:${_olo_asan_runtime_dir}"
                    "clang_rt.asan_dynamic-x86_64.lib"
                    "/wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
                )
                message(STATUS "  clang-cl ASan runtime linked from: ${_olo_asan_runtime_dir}")
            else()
                message(WARNING
                    "clang-cl ASan: could not locate ${_olo_asan_import_lib} (checked: ${_olo_asan_candidates}). "
                    "Linking may fail with undefined __asan_* symbols.")
            endif()

            unset(_olo_asan_import_lib)
            unset(_olo_asan_candidates)
            unset(_olo_rt_dir)
            unset(_olo_res_dir)
            unset(_olo_asan_runtime_dir)
            unset(_olo_cand)
        endif()

        message(STATUS "  MSVC ASan: /fsanitize=address /MD (no leak detection on Windows)")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
        message(STATUS "  GCC/Clang ASan: -fsanitize=address (LSan included automatically on Linux)")
    else()
        message(WARNING "ASan requested but compiler '${CMAKE_CXX_COMPILER_ID}' is not supported")
    endif()
endif()

# --- Leak Sanitizer (standalone, without ASan) ---
if(OLO_ENABLE_LSAN AND NOT OLO_ENABLE_ASAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=leak -fno-omit-frame-pointer)
        add_link_options(-fsanitize=leak)
        message(STATUS "Leak Sanitizer (LSan) enabled (standalone)")
    else()
        message(WARNING "LSan is only supported on GCC/Clang (Linux). Ignoring OLO_ENABLE_LSAN.")
    endif()
elseif(OLO_ENABLE_LSAN AND OLO_ENABLE_ASAN)
    message(STATUS "LSan is already included with ASan on Linux — OLO_ENABLE_LSAN is redundant")
endif()

# --- Undefined Behavior Sanitizer ---
if(OLO_ENABLE_UBSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Select checks most relevant to a game engine:
        # - signed-integer-overflow: common in physics/math code
        # - null: null pointer dereference
        # - alignment: misaligned memory access (SIMD, GPU upload buffers)
        # - float-divide-by-zero: particle systems, normalization
        # - return: end of non-void function without return
        # - unreachable: __builtin_unreachable hit
        # - vla-bound: negative VLA size
        # - shift: out-of-range shift
        set(UBSAN_CHECKS
            signed-integer-overflow,null,alignment,float-divide-by-zero,return,unreachable,vla-bound,shift
        )
        add_compile_options(-fsanitize=undefined -fno-sanitize-recover=${UBSAN_CHECKS} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
        message(STATUS "Undefined Behavior Sanitizer (UBSan) enabled: ${UBSAN_CHECKS}")
    else()
        message(WARNING "UBSan is only supported on GCC/Clang. Ignoring OLO_ENABLE_UBSAN.")
    endif()
endif()

# --- Thread Sanitizer ---
if(OLO_ENABLE_TSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
        message(STATUS "Thread Sanitizer (TSan) enabled — detects data races")
    else()
        message(WARNING "TSan is only supported on GCC/Clang. Ignoring OLO_ENABLE_TSAN.")
    endif()
endif()
