﻿project(OloEngine-LuaScriptCore
		VERSION 0.0.1
		LANGUAGES CXX)

# Main targets built by this project
add_subdirectory(src)

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

target_compile_definitions(OloEngine-LuaScriptCore PRIVATE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Dist>:OLO_DIST>
)

if(MSVC)
	target_compile_options(OloEngine-LuaScriptCore PRIVATE /W4)
else()
	target_compile_options(OloEngine-LuaScriptCore PRIVATE -Wall -Wextra -Wundef -Wno-cast-function-type -pedantic -Wno-long-long -Wshadow -Werror -Wno-error=deprecated-declarations)
endif()