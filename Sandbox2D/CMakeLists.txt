project(Sandbox2D
		LANGUAGES CXX)

set(Sandbox2D_VERSION_MAJOR 0)
set(Sandbox2D_VERSION_MINOR 0)
set(Sandbox2D_VERSION_PATCH 1)
set(Sandbox2D_VERSION
${Sandbox2D_VERSION_MAJOR}.${Sandbox2D_VERSION_MINOR}.${Sandbox2D_VERSION_PATCH}
)

# Main targets built by this project
add_subdirectory(src)

# Configure the Sandbox2D target using our common properties
olo_configure_app(Sandbox2D NO_PCH)

add_dependencies(Sandbox2D
				OloEngine
				OloEngine-ScriptCore
				Sandbox-Scripting
)

# Set up common include directories
olo_set_common_include_directories(Sandbox2D)

target_link_libraries(Sandbox2D
					OloEngine
)
