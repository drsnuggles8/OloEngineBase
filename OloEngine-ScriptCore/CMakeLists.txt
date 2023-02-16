project(OloEngine-ScriptCore
		LANGUAGES CSharp)

set(OloEngine_ScriptCore_VERSION_MAJOR 0)
set(OloEngine_ScriptCore_VERSION_MINOR 1)
set(OloEngine_ScriptCore_VERSION_PATCH 0)
set(OloEngine_ScriptCore_VERSION ${OloEngine_ScriptCore_VERSION_MAJOR}.${OloEngine_ScriptCore_VERSION_MINOR}.${OloEngine_ScriptCore_VERSION_PATCH})

# Main targets built by this project
add_subdirectory(src)

set_target_properties(OloEngine-ScriptCore PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					ARCHIVE_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					CMAKE_DOTNET_TARGET_FRAMEWORK_VERSION "v4.8.1"
					CMAKE_CSharp_FLAGS "/langversion:8"
)

target_compile_definitions(OloEngine-ScriptCore PRIVATE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Dist>:OLO_DIST>
)
