include(../cmake/SetupConfigurations.cmake)

project(OloEngine
		LANGUAGES CXX)

set(OloEngine_VERSION_MAJOR 0)
set(OloEngine_VERSION_MINOR 0)
set(OloEngine_VERSION_PATCH 1)
set(OloEngine_VERSION
${OloEngine_VERSION_MAJOR}.${OloEngine_VERSION_MINOR}.${OloEngine_VERSION_PATCH}
)

# Externally provided content
add_subdirectory(vendor)

# Main targets built by this project
add_subdirectory(src)

set_target_properties(OloEngine PROPERTIES
					  CXX_STANDARD 20
					  CXX_STANDARD_REQUIRED YES
					  CXX_EXTENSIONS NO
					  POSITION_INDEPENDENT_CODE False
					  INTERPROCEDURAL_OPTIMIZATION False
)

# Rather than referring to CMAKE_BUILD_TYPE in
# the CMakeLists.txt file, projects should instead use other more robust alternative techniques, such
# as generator expressions based on $<CONFIG:…>
# NOT SURE IF THIS WHOLE SHIT IS EVEN NEEDED
# FIX THIS SECTION, COMBINE SHIT AND STUFF

#[[
set_target_properties("OloEngine" PROPERTIES
					OUTPUT_NAME "OloEngine"
					ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEngine"
					LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEngine"
					RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEngine"
)
]]

#[[
# Set output directories for all builds
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
	string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
	set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/lib )
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/lib )
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_SOURCE_DIR}/bin )
endforeach( OUTPUTCONFIG CMAKE_CONFIGURATION_TYPES )
]]

# C:\Users\ole\source\repos\OloEngine\build\OloEngine\src\Release\ 

add_dependencies(OloEngine
				glfw
				glad
				ryml
)

target_precompile_headers("OloEngine" PUBLIC src/OloEnginePCH.h)

target_include_directories("OloEngine" PRIVATE
  src
  vendor/spdlog/include
  vendor/GLFW/include
  vendor/Glad/include
  vendor/imgui
  vendor/glm
  vendor/stb_image
  vendor/entt/include
  vendor/ryml/src
)

target_compile_definitions("OloEngine" PRIVATE
  _CRT_SECURE_NO_WARNINGS
  GLFW_INCLUDE_NONE
  $<$<CONFIG:Debug>:OLO_DEBUG>
  $<$<CONFIG:Release>:OLO_RELEASE>
  $<$<CONFIG:Dist>:OLO_DIST>
)

target_compile_options("OloEngine" PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-m64>
  $<$<COMPILE_LANGUAGE:C>:-g>
  $<$<COMPILE_LANGUAGE:CXX>:-m64>
  $<$<COMPILE_LANGUAGE:CXX>:-g>
  $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
)

#target_link_directories("OloEngine" PRIVATE)

target_link_libraries(OloEngine
					  Glad
					  glfw
					  ImGui
					  opengl32.lib
					  ryml
					  stb_image
)
