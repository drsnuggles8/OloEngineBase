project(OloEngine-ScriptCore
		LANGUAGES CSharp)

set(OloEngine-ScriptCore_VERSION_MAJOR 0)
set(OloEngine-ScriptCore_VERSION_MINOR 0)
set(OloEngine-ScriptCore_VERSION_PATCH 1)
set(OloEngine-ScriptCore_VERSION
${OloEngine-ScriptCore_VERSION_MAJOR}.${OloEngine-ScriptCore_VERSION_MINOR}.${OloEngine-ScriptCore_VERSION_PATCH}
)

# Main targets built by this project
add_subdirectory(src)

set_target_properties(OloEngine-ScriptCore PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG OloEditor/Resources/Scripts
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE OloEditor/Resources/Scripts
					ARCHIVE_OUTPUT_DIRECTORY_DIST OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_DEBUG OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_RELEASE OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_DIST OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DEBUG OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_RELEASE OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DIST OloEditor/Resources/Scripts
)

target_compile_definitions(OloEngine-ScriptCore PRIVATE
							"/langversion:6"
							CMAKE_DOTNET_TARGET_FRAMEWORK_VERSION "v4.7.2"
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Dist>:OLO_DIST>
)

if(MSVC)
	target_compile_options(OloEngine-ScriptCore PRIVATE /W4)
else()
	target_compile_options(OloEngine-ScriptCore PRIVATE -Wall -Wextra -pedantic)
endif()