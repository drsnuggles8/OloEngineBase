project(OloEngine-LuaScriptCore
		VERSION 0.0.1
		LANGUAGES CXX)

# Main targets built by this project
add_subdirectory(src)

# Configure standard compiler options, definitions, etc.
olo_set_compiler_options(OloEngine-LuaScriptCore)
olo_set_common_definitions(OloEngine-LuaScriptCore)

# Set custom output directory for script libraries. The DLL is the product and lands in the
# editor's resources; its import library stays in this build tree (#1386,
# docs/agent-rules/worktree-shared-build-outputs.md).
set_target_properties(OloEngine-LuaScriptCore PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG		${CMAKE_BINARY_DIR}/lib/Debug/OloEngine-LuaScriptCore
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE	${CMAKE_BINARY_DIR}/lib/Release/OloEngine-LuaScriptCore
					ARCHIVE_OUTPUT_DIRECTORY_DIST		${CMAKE_BINARY_DIR}/lib/Dist/OloEngine-LuaScriptCore
					LIBRARY_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					LIBRARY_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
					RUNTIME_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts
)
