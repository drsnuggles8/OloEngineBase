project(OloEngine-LuaScriptCore
		VERSION 0.0.1
		LANGUAGES CXX)

# Main targets built by this project
add_subdirectory(src)

# Configure standard compiler options, definitions, etc.
olo_set_compiler_options(OloEngine-LuaScriptCore)
olo_set_common_definitions(OloEngine-LuaScriptCore)

# Set custom output directory for script libraries
set_target_properties(OloEngine-LuaScriptCore PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					ARCHIVE_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
)
