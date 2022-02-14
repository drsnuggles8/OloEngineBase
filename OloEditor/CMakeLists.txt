project(OloEditor
		LANGUAGES CXX)

set(OloEditor_VERSION_MAJOR 0)
set(OloEditor_VERSION_MINOR 0)
set(OloEditor_VERSION_PATCH 1)
set(OloEditor_VERSION
${OloEditor_VERSION_MAJOR}.${OloEditor_VERSION_MINOR}.${OloEditor_VERSION_PATCH}
)

# Main targets built by this project
add_subdirectory(src)

set_target_properties("OloEditor" PROPERTIES
					  CXX_STANDARD 20
					  CXX_STANDARD_REQUIRED YES
					  CXX_EXTENSIONS NO
					  POSITION_INDEPENDENT_CODE False
					  INTERPROCEDURAL_OPTIMIZATION False
)

set_target_properties("OloEditor" PROPERTIES
					ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/OloEditor
					ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/OloEditor
					ARCHIVE_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/OloEditor
					LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/OloEditor
					LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/OloEditor
					LIBRARY_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/OloEditor
					RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/Debug/OloEditor
					RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/Release/OloEditor
					RUNTIME_OUTPUT_DIRECTORY_DIST ${CMAKE_SOURCE_DIR}/bin/Dist/OloEditor
)

add_dependencies(OloEditor
				OloEngine
)

target_include_directories("OloEditor" PRIVATE
							${CMAKE_SOURCE_DIR}/OloEngine/src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
							${CMAKE_SOURCE_DIR}/OloEngine/vendor
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/glm-src
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/entt-src/single_include/entt
							${CMAKE_SOURCE_DIR}/OloEngine/vendor/imgui-src
)

target_compile_definitions("OloEditor" PRIVATE
							$<$<CONFIG:Debug>:OLO_DEBUG>
							$<$<CONFIG:Release>:OLO_RELEASE>
							$<$<CONFIG:Dist>:OLO_DIST>
)

target_link_libraries(OloEditor
					OloEngine
)
