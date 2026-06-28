# =============================================================================
# OloEngine Fuzzing Targets (Layer 11 of the renderer testing pyramid —
# docs/agent-rules/testing-architecture.md, row "L11 Sanitizers & fuzzing")
#
# Enabled by `-DOLO_ENABLE_FUZZING=ON`. Requires a fuzzer-capable compiler:
#   - Clang / ClangCL with `-fsanitize=fuzzer,address`
#   - (MSVC cl.exe does not support libFuzzer — skip if detected)
#
# Each harness exposes a single `LLVMFuzzerTestOneInput` entry point that
# drives a known-attack-surface deserializer with arbitrary input bytes.
# Corpus files live under `corpus/<harness>/` and are checked in so the
# fuzzer has a known-good starting set (both valid files and known past
# crashers).
#
# The actual sanitizer / runtime-lib wiring (and the platform-specific dance
# needed for clang-cl + lld-link on Windows) lives in `cmake/Fuzzing.cmake`.
# That module is included at the top-level CMakeLists.txt so its compile
# flags propagate to the OloEngine static lib too — the harnesses can't
# link cleanly otherwise (STL annotation ABI mismatch via /failifmismatch).
# =============================================================================

# OLO_ENABLE_FUZZING is declared in cmake/Fuzzing.cmake. If it's off, or the
# compiler isn't Clang, that module has already early-returned and the
# `olo_apply_fuzzer_link` helper is a no-op — we still skip target creation
# here so we don't spam the cache with disabled targets.
if(NOT OLO_ENABLE_FUZZING)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    return()
endif()

function(olo_add_fuzzer NAME)
    set(_oneValue "")
    set(_multiValue SOURCES CORPUS)
    cmake_parse_arguments(F "" "${_oneValue}" "${_multiValue}" ${ARGN})

    add_executable(${NAME} ${F_SOURCES})
    target_link_libraries(${NAME} PRIVATE OloEngine)
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/OloEngine/src
        ${spdlog_SOURCE_DIR}/include
    )

    # Per-target libFuzzer + runtime-lib link wiring (Windows ClangCL needs
    # explicit clang_rt.* libs; non-Windows uses the clang driver).
    olo_apply_fuzzer_link(${NAME})

    # A short smoke-run target: 30 s, corpus dir preloaded, artifacts written
    # next to the binary. Invoked from CI via `cmake --build --target ${NAME}-smoke`.
    add_custom_target(${NAME}-smoke
        COMMAND $<TARGET_FILE:${NAME}> -max_total_time=30 -print_final_stats=1
                ${CMAKE_CURRENT_SOURCE_DIR}/${F_CORPUS}
        DEPENDS ${NAME}
        USES_TERMINAL
        COMMENT "Fuzzing ${NAME} for 30 seconds"
    )
endfunction()

olo_add_fuzzer(FuzzAnimationBinary
    SOURCES FuzzAnimationBinary.cpp
    CORPUS corpus/animation_binary
)

olo_add_fuzzer(FuzzMeshBinary
    SOURCES FuzzMeshBinary.cpp
    CORPUS corpus/mesh_binary
)

olo_add_fuzzer(FuzzInputActionYaml
    SOURCES FuzzInputActionYaml.cpp
    CORPUS corpus/input_action_yaml
)

olo_add_fuzzer(FuzzImageDecoder
    SOURCES FuzzImageDecoder.cpp
    CORPUS corpus/image_decoder
)

olo_add_fuzzer(FuzzSceneYaml
    SOURCES FuzzSceneYaml.cpp
    CORPUS corpus/scene_yaml
)

olo_add_fuzzer(FuzzAssimpMesh
    SOURCES FuzzAssimpMesh.cpp
    CORPUS corpus/assimp_mesh
)

olo_add_fuzzer(FuzzSpirvCross
    SOURCES FuzzSpirvCross.cpp
    CORPUS corpus/spirv_cross
)

# Aggregate target that runs every fuzzer smoke for the full CI budget.
add_custom_target(OloEngine-FuzzSmoke
    DEPENDS FuzzAnimationBinary-smoke FuzzMeshBinary-smoke FuzzInputActionYaml-smoke
            FuzzImageDecoder-smoke FuzzSceneYaml-smoke
            FuzzAssimpMesh-smoke FuzzSpirvCross-smoke
)

# Build-only aggregate — compiles every harness without running them. Handy
# for `cmake --build --target OloEngine-Fuzz` in developer workflows where a
# smoke run would blow the iteration budget.
add_custom_target(OloEngine-Fuzz
    DEPENDS FuzzAnimationBinary FuzzMeshBinary FuzzInputActionYaml
            FuzzImageDecoder FuzzSceneYaml
            FuzzAssimpMesh FuzzSpirvCross
)
