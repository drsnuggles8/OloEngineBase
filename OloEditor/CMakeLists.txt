project(OloEditor
		VERSION 0.0.1
		LANGUAGES CXX)

# Main targets built by this project
add_subdirectory(src)

set_target_properties(OloEditor PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/bin/Debug/OloEditor
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/bin/Release/OloEditor
					ARCHIVE_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/bin/Dist/OloEditor
					LIBRARY_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/bin/Debug/OloEditor
					LIBRARY_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/bin/Release/OloEditor
					LIBRARY_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/bin/Dist/OloEditor
					RUNTIME_OUTPUT_DIRECTORY_DEBUG		${CMAKE_SOURCE_DIR}/bin/Debug/OloEditor
					RUNTIME_OUTPUT_DIRECTORY_RELEASE	${CMAKE_SOURCE_DIR}/bin/Release/OloEditor
					RUNTIME_OUTPUT_DIRECTORY_DIST		${CMAKE_SOURCE_DIR}/bin/Dist/OloEditor
					VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

check_ipo_supported(RESULT LTO_SUPPORT OUTPUT output)
if(LTO_SUPPORT)
	message(STATUS "-- Enabled Link-Time Optimization (LTO)!")
	set_target_properties(OloEditor PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
	message(WARNING "IPO is not supported: ${output}")
endif()

add_dependencies(OloEditor
				OloEngine
				OloEngine-ScriptCore
				Sandbox-Scripting
				png_shared
				zlib
)

target_include_directories(OloEditor PRIVATE
							${CMAKE_SOURCE_DIR}/OloEngine/src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/entt-src/single_include/entt
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/filewatch-src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/glm-src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/imgui-src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/imguizmo-src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/sol2-src/include
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
)

target_compile_definitions(OloEditor PRIVATE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Release>:TRACY_ENABLE>
							$<$<CONFIG:Release>:TRACY_ON_DEMAND>
							$<$<CONFIG:Dist>:OLO_DIST>
)

if(MSVC)
	target_link_options(OloEditor PRIVATE
								$<$<CONFIG:Release>:/INCREMENTAL:NO>
								$<$<CONFIG:Release>:/DEBUG>
								$<$<CONFIG:Release>:/OPT:REF> # Remove unreferenced functions and data
								$<$<CONFIG:Release>:/OPT:ICF> # Identical COMDAT folding
	)
	endif()

target_link_libraries(OloEditor
					OloEngine
)

if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
	target_compile_options(OloEditor PRIVATE /W4)
else()
	target_compile_options(OloEditor PRIVATE -Wall -Wextra -Wundef -Wno-cast-function-type -pedantic -Wno-long-long -Wshadow -Werror -Wno-error=deprecated-declarations)
endif()