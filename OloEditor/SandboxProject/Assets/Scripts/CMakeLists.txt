project(Sandbox-Scripting
		LANGUAGES CSharp)

set(Sandbox-Scripting_VERSION_MAJOR 0)
set(Sandbox-Scripting_VERSION_MINOR 1)
set(Sandbox-Scripting_VERSION_PATCH 0)
set(Sandbox-Scripting_VERSION ${Sandbox-Scripting_VERSION_MAJOR}.${Sandbox-Scripting_VERSION_MINOR}.${Sandbox-Scripting_VERSION_PATCH})

# Main targets built by this project
add_subdirectory(Source)
add_dependencies(Sandbox-Scripting
				OloEngine-ScriptCore
)

set_target_properties(Sandbox-Scripting PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					ARCHIVE_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					LIBRARY_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					LIBRARY_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					LIBRARY_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					RUNTIME_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					RUNTIME_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					RUNTIME_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/SandboxProject/Assets/Scripts/Binaries
					CMAKE_DOTNET_TARGET_FRAMEWORK_VERSION "v4.8.1"
					CMAKE_CSharp_FLAGS "/langversion:8"
)

target_compile_definitions(Sandbox-Scripting PRIVATE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Dist>:OLO_DIST>
)

set_target_properties(Sandbox-Scripting PROPERTIES
						VS_DOTNET_REFERENCES "${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts/OloEngine-ScriptCore.dll"
)

target_include_directories("Sandbox-Scripting" PRIVATE
						${CMAKE_SOURCE_DIR}/OloEngine-ScriptCore
)
