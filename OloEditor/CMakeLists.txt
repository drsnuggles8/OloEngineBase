project(OloEditor
		VERSION 0.0.1
		LANGUAGES CXX)

# Main targets built by this project
add_subdirectory(src)

# Configure the OloEditor target using our common properties
olo_configure_app(OloEditor NO_PCH)

add_dependencies(OloEditor
				OloEngine
				OloEngine-ScriptCore
				Sandbox-Scripting
)
# Note: png_shared and zlib are now dependencies of OloEngine, so they're implicitly included

# Set up common include directories
olo_set_common_include_directories(OloEditor)

target_link_libraries(OloEditor
					OloEngine
)