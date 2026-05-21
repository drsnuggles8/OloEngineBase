project(OloServer
        VERSION 0.0.1
        LANGUAGES CXX)

add_subdirectory(src)

# Configure the OloServer target using our common properties
olo_configure_app(OloServer)

# Set up common include directories
olo_set_common_include_directories(OloServer)

# target_link_libraries implies a build-order dependency on OloEngine, so no
# separate add_dependencies(... OloEngine) is needed.
target_link_libraries(OloServer
                    OloEngine
)

target_compile_definitions(OloServer PRIVATE
    OLO_HEADLESS=1
)

# FOLDER is assigned at the root CMakeLists.txt to keep app folder grouping in one place.
