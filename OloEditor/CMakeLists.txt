project(OloEditor
		VERSION 0.0.1
		LANGUAGES CXX)

# Main targets built by this project
add_subdirectory(src)

# Configure the OloEditor target using our common properties
olo_configure_app(OloEditor)

# Set up common include directories
olo_set_common_include_directories(OloEditor)

# OloEditor sources include each other with `Preview/...`, `Panels/...` etc.
# paths rooted at OloEditor/src — make that work regardless of where the
# including file lives in the tree.
target_include_directories(OloEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# target_link_libraries implies a build-order dependency on OloEngine, so no
# separate add_dependencies(... OloEngine) is needed.
target_link_libraries(OloEditor
                    OloEngine
)

# Suppress MSVC's "use the _s variant" CRT warnings (C4996 on strncpy/sscanf/etc.).
# OloEngine already adopts the same stance for the engine library; matching here
# keeps the editor consistent and avoids per-call-site noise across the panels.
target_compile_definitions(OloEditor PRIVATE _CRT_SECURE_NO_WARNINGS)
