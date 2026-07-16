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

# Custom target to run the tool
add_custom_target(GenerateBindings
    COMMAND OloHeaderTool "${OHT_SCAN_DIR}" "${OHT_CPP_OUT}" "${OHT_CS_OUT}" "${OHT_SCENE_OUT}" "${OHT_SAVEGAME_OUT}" "${OHT_MCP_OUT}"
    DEPENDS OloHeaderTool
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running OloHeaderTool to generate scripting bindings, the AllComponents tuple, the save-game component lists, and the MCP field registry..."
    VERBATIM
)
