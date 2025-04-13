project(Sandbox3D
		LANGUAGES CXX)

set(Sandbox3D_VERSION_MAJOR 0)
set(Sandbox3D_VERSION_MINOR 0)
set(Sandbox3D_VERSION_PATCH 1)
set(Sandbox3D_VERSION
${Sandbox3D_VERSION_MAJOR}.${Sandbox3D_VERSION_MINOR}.${Sandbox3D_VERSION_PATCH}
)

# Main targets built by this project
add_subdirectory(src)

# Configure the Sandbox3D target using our common properties
olo_configure_app(Sandbox3D NO_PCH)

add_dependencies(Sandbox3D
				OloEngine
				OloEngine-ScriptCore
				Sandbox-Scripting
)

# Set up common include directories
olo_set_common_include_directories(Sandbox3D)

target_link_libraries(Sandbox3D
					OloEngine
)
