project(Sandbox
		LANGUAGES CXX)

set(Sandbox_VERSION_MAJOR 0)
set(Sandbox_VERSION_MINOR 0)
set(Sandbox_VERSION_PATCH 1)
set(Sandbox_VERSION
${Sandbox_VERSION_MAJOR}.${Sandbox_VERSION_MINOR}.${Sandbox_VERSION_PATCH}
)

# Main targets built by this project
add_subdirectory(src)

set_target_properties(Sandbox PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/Sandbox
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/Sandbox
					ARCHIVE_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/Sandbox
					LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/Sandbox
					LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/Sandbox
					LIBRARY_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/Sandbox
					RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/Sandbox
					RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/Sandbox
					RUNTIME_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/Sandbox
					VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_dependencies(Sandbox
				OloEngine
)

target_include_directories("Sandbox" PRIVATE
							${CMAKE_SOURCE_DIR}/OloEngine/src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
							${CMAKE_SOURCE_DIR}/OloEngine/vendor
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/glm-src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/entt-src/single_include/entt
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/imgui-src
)

target_compile_definitions("Sandbox" PRIVATE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Dist>:OLO_DIST>
)

if(MSVC)
	target_compile_definitions("Sandbox" PRIVATE /W4)
else()
	target_compile_definitions("Sandbox" PRIVATE -Wall -Wextra -pedantic)
endif()

target_link_libraries(Sandbox
					OloEngine
)
