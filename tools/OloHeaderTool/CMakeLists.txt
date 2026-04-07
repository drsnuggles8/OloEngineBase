project(OloHeaderTool LANGUAGES CXX)

add_executable(OloHeaderTool main.cpp)
target_compile_features(OloHeaderTool PRIVATE cxx_std_20)
set_target_properties(OloHeaderTool PROPERTIES FOLDER "Tools")

# Define the output paths for generated files
set(OHT_SCAN_DIR "${CMAKE_SOURCE_DIR}/OloEngine/src" CACHE PATH "Directory to scan for OLO_PROPERTY annotations")
set(OHT_CPP_OUT  "${CMAKE_SOURCE_DIR}/OloEngine/src/OloEngine/Scripting/C#/Generated" CACHE PATH "C++ generated output directory")
set(OHT_CS_OUT   "${CMAKE_SOURCE_DIR}/OloEngine-ScriptCore/src/OloEngine" CACHE PATH "C# generated output directory")

# Custom target to run the tool
add_custom_target(GenerateBindings
    COMMAND OloHeaderTool "${OHT_SCAN_DIR}" "${OHT_CPP_OUT}" "${OHT_CS_OUT}"
    DEPENDS OloHeaderTool
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running OloHeaderTool to generate scripting bindings..."
    VERBATIM
)
