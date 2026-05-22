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
