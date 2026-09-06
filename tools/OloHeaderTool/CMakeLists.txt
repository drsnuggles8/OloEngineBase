project(OloHeaderTool LANGUAGES CXX)

add_executable(OloHeaderTool main.cpp)
# OloHeaderTool is a small build-time tool; nothing in main.cpp requires C++23,
# so pin it explicitly at C++20 instead of inheriting the global C++23 baseline.
# Setting per-target standard properties (rather than `cxx_std_20` features)
# both lowers the minimum AND caps the standard used to compile this target.
set_target_properties(OloHeaderTool PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    FOLDER "Tools"
)

# Define the output paths for generated files
set(OHT_SCAN_DIR  "${CMAKE_SOURCE_DIR}/OloEngine/src" CACHE PATH "Directory to scan for OLO_PROPERTY annotations and *Component definitions")
set(OHT_CPP_OUT   "${CMAKE_SOURCE_DIR}/OloEngine/src/OloEngine/Scripting/C#/Generated" CACHE PATH "C++ generated output directory")
set(OHT_CS_OUT    "${CMAKE_SOURCE_DIR}/OloEngine-ScriptCore/src/OloEngine" CACHE PATH "C# generated output directory")
# Scene-domain generated output (the AllComponents tuple) — kept separate from the
# scripting bindings so Components.h can #include it with a clean path.
set(OHT_SCENE_OUT "${CMAKE_SOURCE_DIR}/OloEngine/src/OloEngine/Scene/Generated" CACHE PATH "Scene generated output directory (AllComponents tuple)")
# Save-game generated output (the SAVE_COMPONENT / TRY_LOAD_COMPONENT enumeration
# lists) — kept next to SaveGameSerializer.cpp so it can #include them by path.
set(OHT_SAVEGAME_OUT "${CMAKE_SOURCE_DIR}/OloEngine/src/OloEngine/SaveGame/Generated" CACHE PATH "Save-game generated output directory (component capture/restore lists)")
# Editor MCP generated output (the olo_entity_set_field writable-field registry,
# issue #607) — lives next to McpGenericFieldWrite.h, which #includes it inside
# BuildRegistry(). This is the one generated artefact that lands in OloEditor/
# rather than OloEngine/; the editor and the test binary both compile the header.
set(OHT_MCP_OUT "${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/Generated" CACHE PATH "Editor MCP generated output directory (writable-field registry)")
# Visual-script generated output (the engine-side component-field registry behind
# the Get/Set Field nodes, issue #793) - the ENGINE sibling of OHT_MCP_OUT above.
# It cannot share that directory: a graph runs in OloRuntime and OloServer, which
# do not link OloEditor, so the registry has to live under OloEngine/src.
set(OHT_VS_OUT "${CMAKE_SOURCE_DIR}/OloEngine/src/OloEngine/Scripting/VisualScript/Generated" CACHE PATH "Visual-script generated output directory (engine-side component-field registry)")

# ── Fail-fast guards (issue #758) ────────────────────────────────────────────
# The issue asks for `find_program`; the honest equivalent here is a target check,
# because the generator is built in-tree and `find_program` would only ever look
# for something CMake itself just declared. What matters is the FAILURE MODE both
# guards protect: a generator that never ran, or ran over the wrong tree, produces
# stale or empty generated files, and that surfaces much later as a coverage-test
# *parse-count assertion* rather than as a build error. Catch it at configure time.
if(NOT TARGET OloHeaderTool)
    message(FATAL_ERROR "OloHeaderTool target was not created — the codegen generator is missing.")
endif()
if(NOT IS_DIRECTORY "${OHT_SCAN_DIR}")
    message(FATAL_ERROR
        "OHT_SCAN_DIR does not exist: ${OHT_SCAN_DIR}\n"
        "OloHeaderTool would scan nothing and every generated component list would "
        "be empty. Point OHT_SCAN_DIR at the engine source root.")
endif()

# ── Generated artefacts ──────────────────────────────────────────────────────
# The exhaustive list of what main.cpp writes, in its five output directories.
# Kept in sync with the tool by OloHeaderToolOutputsTest, which greps main.cpp for
# its own writes and fails if this list drifts in either direction.
#
# This list is documentation and a test anchor — it is deliberately NOT passed to
# the custom command's BYPRODUCTS; see the rule below for why.
set(OHT_GENERATED_FILES
    "${OHT_SCENE_OUT}/AllComponents.Generated.inl"
    "${OHT_SCENE_OUT}/OnComponentAdded.Generated.inl"
    "${OHT_SCENE_OUT}/OnComponentRemoved.Generated.inl"
    "${OHT_SCENE_OUT}/SceneSerializeComponents.Generated.inl"
    "${OHT_SCENE_OUT}/SceneDeserializeComponents.Generated.inl"
    "${OHT_SCENE_OUT}/SceneBinaryWriteComponents.Generated.inl"
    "${OHT_SCENE_OUT}/SceneBinaryReadComponents.Generated.inl"
    "${OHT_SCENE_OUT}/SceneBinaryCoveredComponents.Generated.inl"
    "${OHT_SAVEGAME_OUT}/SaveGameComponentCapture.Generated.inl"
    "${OHT_SAVEGAME_OUT}/SaveGameComponentRestore.Generated.inl"
    "${OHT_CPP_OUT}/ScriptGlueBindings.inl"
    "${OHT_CPP_OUT}/ScriptGlueRegistrations.inl"
    "${OHT_CS_OUT}/Scene/Components.Generated.cs"
    "${OHT_CS_OUT}/InternalCalls.Generated.cs"
    "${OHT_MCP_OUT}/McpFieldRegistry.Generated.inl"
    "${OHT_VS_OUT}/ComponentFieldRegistry.Generated.inl"
)

# ── Build-graph integration (issue #758) ─────────────────────────────────────
# This used to be an `add_custom_target`, which declares no outputs and tracks no
# inputs — so the full recursive scan of OloEngine/src sat on the critical path of
# every incremental build, and the build graph had no idea the rule wrote into the
# shared source tree.
#
# The declared OUTPUT is a STAMP, not the generated files, and that is the crux of
# the design. The tool writes each generated file only when its CONTENT changes
# (WriteIfChanged) — which is exactly what stops a one-line header edit from
# recompiling the whole engine through Components.h. But a content-stable output is
# unusable as a build-graph output: after a header edit the unchanged .inl stays
# OLDER than that header, so the rule never settles and the scan re-runs on every
# build forever. (Ninja alone would survive this via `restat`; MSBuild's tlog
# up-to-date check has no equivalent, so under the VS generator it would be a
# permanent re-run.) The stamp is the one output that is always refreshed.
#
# The generated files are NOT declared as BYPRODUCTS either, and that is the more
# important half. They are **tracked in git**, not build-tree artefacts: CMake
# treats byproducts as cleanable output (the Makefile generators delete BYPRODUCTS
# and other GENERATED files on `make clean`, and they become edge outputs that
# `ninja -t clean` removes), so declaring them would let a routine `--target clean`
# delete fifteen committed source files out of the working tree. A stamp is the
# only output this rule should own.
#
# Nothing is lost by omitting them. Ordering — "codegen runs before anything that
# includes the .inl" — comes from `add_dependencies(OloEngine GenerateBindings)`
# in the root CMakeLists, not from the outputs list, and the files are present in
# a fresh checkout because they are committed. To force a regeneration, delete
# `OloHeaderTool.stamp` from the build tree rather than the generated sources.
set(OHT_STAMP   "${CMAKE_CURRENT_BINARY_DIR}/OloHeaderTool.stamp")
set(OHT_DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/OloHeaderTool.d")

# ── The two things the depfile cannot track ──────────────────────────────────
# The depfile the tool emits lists every header it READ, so editing one re-runs
# codegen. Two cases it cannot carry, both handled from this one glob:
#
#  1. ADDING or DELETING a header. The depfile can only name paths that existed
#     during the last run. The classic trick is to also list every visited
#     DIRECTORY, since a directory's mtime moves when an entry is created or
#     removed inside it — and under Ninja that works. Under the Visual Studio
#     generator it is fatal: CMake funnels the depfile into MSBuild's
#     AdditionalInputs, whose up-to-date check can never satisfy a directory
#     entry, so the rule is out of date on EVERY build. Measured on VS 18 2026:
#     4.3 s per no-op build with directories listed, 1.0 s without.
#
#     Instead, the sorted header list is written to a manifest below. Because
#     configure_file only rewrites when the content differs, the manifest's
#     timestamp moves exactly when the header SET changes — and never on a plain
#     edit, which the depfile already covers. One extra DEPENDS entry, no
#     per-header bloat, and identical behaviour on both generators.
#
#  2. A header whose path contains '#' or '$'. CMake's `cmake_transform_depfile`
#     un-escapes both on read and does not re-escape them on write, after which
#     ninja splits the path at the '#' into two fragments that do not exist — and
#     an edge with a permanently-missing input re-runs forever, silently. The tool
#     omits such paths (reporting each one on stdout) and they are re-added as
#     ordinary DEPENDS here, which both generators handle correctly.
#     `OloEngine/src/OloEngine/Scripting/C#/` is the live case in this repo.
#
# The glob is derived rather than hard-coded so a second '#' directory cannot open
# the same hole unnoticed. CONFIGURE_DEPENDS is what makes case 1 work at all: it
# re-globs at build time and reconfigures when the set changed. That reconfigure is
# the price of catching an added header without re-scanning on every build.
file(GLOB_RECURSE OHT_ALL_SCANNED_HEADERS CONFIGURE_DEPENDS
    "${OHT_SCAN_DIR}/*.h"
    "${OHT_SCAN_DIR}/*.hpp"
)
list(SORT OHT_ALL_SCANNED_HEADERS)

set(OHT_INPUT_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/OloHeaderToolInputs.manifest")
string(REPLACE ";" "\n" _oht_manifest_body "${OHT_ALL_SCANNED_HEADERS}")
file(WRITE "${OHT_INPUT_MANIFEST}.in" "${_oht_manifest_body}\n")
# COPYONLY copies only when the content differs — that write-if-changed step is the
# whole mechanism. A plain file(WRITE) would touch the manifest on every configure
# and drag codegen along with it.
configure_file("${OHT_INPUT_MANIFEST}.in" "${OHT_INPUT_MANIFEST}" COPYONLY)

set(OHT_DEPFILE_UNREPRESENTABLE "")
foreach(_oht_header IN LISTS OHT_ALL_SCANNED_HEADERS)
    if(_oht_header MATCHES "[#$]")
        list(APPEND OHT_DEPFILE_UNREPRESENTABLE "${_oht_header}")
    endif()
endforeach()
list(LENGTH OHT_DEPFILE_UNREPRESENTABLE _oht_unrepresentable_count)
message(STATUS
    "OloHeaderTool: ${_oht_unrepresentable_count} scanned header(s) carry a '#'/'$' in their path "
    "and are tracked via explicit DEPENDS rather than the depfile")

# ── Why there is no `CODEGEN` here, despite issue #758 asking for it ─────────
# `add_custom_command(... CODEGEN)` (CMake 3.31+) registers a rule with CMake's
# global `codegen` target. It is deliberately NOT used, for two independent
# reasons, both measured against CMake 4.2 rather than assumed:
#
#   1. CMake documents CODEGEN as supported by the Ninja and Makefile generators
#      ONLY — Visual Studio generators ignore it outright. This repo's primary
#      tree (build/) is VS 18 2026, so `codegen` would exist in build-clang/ and
#      not in build/, and the "how do I regenerate the bindings" answer would
#      differ per tree.
#   2. More decisively: CODEGEN only attaches a rule to the `codegen` target when
#      the custom command's OUTPUT is consumed by a COMPILED target defined in
#      the SAME DIRECTORY as the command. Neither `add_custom_target(... DEPENDS)`
#      nor that target's SOURCES qualifies, and neither does a `target_sources()`
#      attachment to a target declared in another directory — verified all three
#      ways. The rule lives here with the tool; `OloEngine` is declared in
#      OloEngine/. Satisfying the constraint would mean relocating this whole
#      block into the engine's own CMakeLists, away from the generator it drives.
#
# The failure mode if you try it anyway is quiet: CMake accepts the keyword with
# no error and no warning, `codegen` is emitted as an EMPTY phony, and
# `cmake --build … --target codegen` reports success while regenerating nothing.
# (It also needs `cmake_policy(SET CMP0171 NEW)` at TOP-LEVEL scope — setting it
# in this subdirectory alone leaves `--target codegen` an unknown target.)
#
# `--target GenerateBindings` is the portable equivalent and is strictly better
# here: it works under both generators and, now that this is a real custom
# command, it regenerates without building the engine — which is the whole point
# the `codegen` target would have served.

add_custom_command(
    OUTPUT "${OHT_STAMP}"
    COMMAND OloHeaderTool "${OHT_SCAN_DIR}" "${OHT_CPP_OUT}" "${OHT_CS_OUT}" "${OHT_SCENE_OUT}" "${OHT_SAVEGAME_OUT}" "${OHT_MCP_OUT}" "${OHT_VS_OUT}"
            --depfile "${OHT_DEPFILE}" --stamp "${OHT_STAMP}"
    DEPENDS OloHeaderTool "${OHT_INPUT_MANIFEST}" ${OHT_DEPFILE_UNREPRESENTABLE}
    DEPFILE "${OHT_DEPFILE}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running OloHeaderTool to generate scripting bindings, the AllComponents tuple, the save-game component lists, and the MCP + visual-script field registries..."
    VERBATIM
)

# The target name is kept so the existing `add_dependencies(OloEngine GenerateBindings)`
# / `OloEngine-ScriptCore` edges in the root CMakeLists keep working unchanged. It is
# now an empty target that merely DEPENDS on the stamp, so depending on it orders
# consumers after codegen without forcing the scan to re-run.
add_custom_target(GenerateBindings DEPENDS "${OHT_STAMP}")
set_target_properties(GenerateBindings PROPERTIES FOLDER "Tools")
