project(OloServer
        VERSION 0.0.1
        LANGUAGES CXX)

add_subdirectory(src)

# Configure the OloServer target using our common properties
olo_configure_app(OloServer NO_PCH)

add_dependencies(OloServer
                OloEngine
)

# Set up common include directories
olo_set_common_include_directories(OloServer)

target_link_libraries(OloServer
                    OloEngine
)

target_compile_definitions(OloServer PRIVATE
    OLO_HEADLESS=1
)

set_target_properties(OloServer PROPERTIES FOLDER "Applications")
