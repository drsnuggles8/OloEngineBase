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

# --- Address Sanitizer ---
if(OLO_ENABLE_ASAN)
    message(STATUS "Address Sanitizer (ASan) enabled")

    if(MSVC)
        add_compile_options(/fsanitize=address)

        # ASan is incompatible with /RTC1 (runtime checks) and incremental linking.
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTCs" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTCu" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        string(REPLACE "/RTCs" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        string(REPLACE "/RTCu" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
        add_link_options(/INCREMENTAL:NO)

        message(STATUS "  MSVC ASan: /fsanitize=address (no leak detection on Windows)")
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
