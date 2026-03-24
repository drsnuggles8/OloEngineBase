project(OloRuntime
        VERSION 0.0.1
        LANGUAGES CXX)

add_subdirectory(src)

# Configure the OloRuntime target using our common properties
olo_configure_app(OloRuntime NO_PCH)

# Set up common include directories
olo_set_common_include_directories(OloRuntime)

target_link_libraries(OloRuntime
                    OloEngine
)

set_target_properties(OloRuntime PROPERTIES FOLDER "Applications")

# In Dist (distribution) builds, hide the console window — players should only
# see the game window.  We keep the standard main() entry point via /ENTRY.
if(WIN32 AND MSVC)
    target_link_options(OloRuntime PRIVATE
        $<$<CONFIG:Dist>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:Dist>:/ENTRY:mainCRTStartup>
    )
endif()
