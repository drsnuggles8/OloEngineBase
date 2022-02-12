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

#Rather than referring to CMAKE_BUILD_TYPE in
#the CMakeLists.txt file, projects should instead use other more robust alternative techniques, such
#as generator expressions based on $<CONFIG:…>
#NOT SURE IF THIS WHOLE SHIT IS EVEN NEEDED
# FIX THIS SECTION, COMBINE SHIT AND STUFF

if(CMAKE_BUILD_TYPE STREQUAL Debug)
  set_target_properties("OloEditor" PROPERTIES
	OUTPUT_NAME "OloEditor"
	ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEditor"
	LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEditor"
	RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEditor"
  )
endif()
if(CMAKE_BUILD_TYPE STREQUAL Release)
  set_target_properties("OloEditor" PROPERTIES
	OUTPUT_NAME "OloEditor"
	ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/OloEditor"
	LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/OloEditor"
	RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/OloEditor"
  )
endif()
if(CMAKE_BUILD_TYPE STREQUAL Dist)
  set_target_properties("OloEditor" PROPERTIES
	OUTPUT_NAME "OloEditor"
	ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/OloEditor"
	LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/OloEditor"
	RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/OloEditor"
  )
endif()


add_dependencies(OloEngine
)

target_include_directories("OloEditor" PRIVATE
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
