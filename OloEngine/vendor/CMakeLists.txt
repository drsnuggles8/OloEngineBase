include(FetchContent)
set(FETCHCONTENT_QUIET off)
set(FETCHCONTENT_BASE_DIR ${PROJECT_SOURCE_DIR}/vendor CACHE PATH "Missing description." FORCE)


message(STATUS "Downloading third party libraries using CMake FetchContent")

# entt - entity component system
FetchContent_Declare(
	entt
	URL https://github.com/skypjack/entt/archive/master.zip
	)
FetchContent_MakeAvailable(entt)
message(STATUS "${Green} entt Should Be Downloaded ${ColourReset}")

# glad - GL/GLES/EGL/GLX/WGL Loader-Generator
FetchContent_Declare(
	glad
	GIT_REPOSITORY https://github.com/Dav1dde/glad.git
	GIT_TAG v0.1.36
	)
FetchContent_GetProperties(glad)
if(NOT glad_POPULATED)
	FetchContent_Populate(glad)
	set(
		GLAD_PROFILE "core"
		CACHE STRING "OpenGL profile"
	)
	set(
		GLAD_GENERATOR "c"
		CACHE STRING "Language to generate the binding for"
	)
	add_subdirectory(
		${glad_SOURCE_DIR}
		${glad_BINARY_DIR}
	)
endif()
message(STATUS "${Green} glad Should Be Downloaded ${ColourReset}")

# glfw - platform-independent API for window & context management
FetchContent_Declare(
	glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
	)
FetchContent_GetProperties(glfw)
if(NOT glfw_POPULATED)
	FetchContent_Populate(glfw)
	set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	add_subdirectory(${glfw_SOURCE_DIR} ${glfw_BINARY_DIR})
endif()
message(STATUS "${Green} glfw Should Be Downloaded ${ColourReset}")

# glm - header-only mathematics library 
FetchContent_Declare(
	glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG		   0.9.9.8
)
FetchContent_GetProperties(glm)
if(NOT glm_POPULATED)
	FetchContent_Populate(glm)
	set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
	add_subdirectory(${glm_SOURCE_DIR} ${glm_BINARY_DIR})
endif()
message(STATUS "${Green} GLM Should Be Downloaded ${ColourReset}")

# imgui - gui library for C++
FetchContent_Declare(
	imgui
	GIT_REPOSITORY https://github.com/ocornut/imgui.git
	GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_PATH  "imgui-src")
	
file(GLOB IMGUI_SOURCES ${IMGUI_PATH}/*.cpp)
file(GLOB IMGUI_HEADERS ${IMGUI_PATH}/*.h)  
add_library("imgui" STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS} "${IMGUI_PATH}/backends/imgui_impl_opengl3.cpp" "${IMGUI_PATH}/backends/imgui_impl_opengl3.h" "${IMGUI_PATH}/backends/imgui_impl_opengl3_loader.h")
target_compile_definitions("imgui" PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD=1)
target_include_directories("imgui" PUBLIC ${IMGUI_PATH} "glfw-src/include" "glad-build/include")
set_target_properties("imgui"
	PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
	LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
	RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
)
message(STATUS "${Green} ImGUI Should Be Downloaded ${ColourReset}")

# imguizmo - guizmo extension for C++ 
FetchContent_Declare(
	imguizmo
	GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
)
FetchContent_MakeAvailable(imguizmo)
set(IMGUIZMO_PATH  "imguizmo-src")
file(GLOB IMGUIZMO_SOURCES ${IMGUIZMO_PATH}/*.cpp)
file(GLOB IMGUIZMO_HEADERS ${IMGUIZMO_PATH}/*.h)  
add_library("imguizmo" STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
target_include_directories("imguizmo" PUBLIC ${IMGUI_PATH})
set_target_properties(imguizmo
	PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
	LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
	RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
)
message(STATUS "${Green} imguizmo Should Be Downloaded ${ColourReset}")

# yaml-cpp - a YAML parser and emitter
FetchContent_Declare(yaml-cpp
	GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
	GIT_TAG master
)
set(YAML_CPP_BUILD_TESTS OFF)
FetchContent_MakeAvailable(yaml-cpp)
message(STATUS "${Green} yaml-cpp Should Be Downloaded ${ColourReset}")

# ryml - yaml-cpp alternative, currently not being used
#FetchContent_Declare(ryml
#	GIT_REPOSITORY https://github.com/biojppm/rapidyaml.git
#	GIT_TAG master
#	GIT_SHALLOW FALSE  # ensure submodules are checked out
#)
#FetchContent_MakeAvailable(ryml)
#message(STATUS "${Green} ryml Should Be Downloaded ${ColourReset}")

# spdlog - logging library
FetchContent_Declare(spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG v1.9.2
)
FetchContent_MakeAvailable(spdlog)
message(STATUS "${Green} spdlog Should Be Downloaded ${ColourReset}")

# stb_image - image loader
set(libStbImage stb_image)
FetchContent_Declare(${libStbImage}
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG origin/master
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
)
FetchContent_GetProperties(${libStbImage})
if(NOT ${libStbImage})
  FetchContent_Populate(${libStbImage})
endif()
add_library(${libStbImage} INTERFACE)
file(
COPY
	${${libStbImage}_SOURCE_DIR}/stb_image.h
DESTINATION
	"${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image"
)
set(STBIMAGE_INSTALL_DIR "${PROJECT_SOURCE_DIR}/vendor/stb_image-build")
message(STATUS "${Green} stb_image Should Be Downloaded ${ColourReset}")

#set_target_properties(ryml PROPERTIES FOLDER "Utilities/ryml")
#set_target_properties(ryml-uninstall PROPERTIES FOLDER "Utilities/ryml")
#set_target_properties(c4core PROPERTIES FOLDER "Utilities/ryml")
#set_target_properties(c4core-amalgamate PROPERTIES FOLDER "Utilities/ryml")
set_target_properties(aob PROPERTIES FOLDER "Utilities/entt")
set_target_properties(glad PROPERTIES FOLDER "Utilities/glad")
set_target_properties(glad-generate-files PROPERTIES FOLDER "Utilities/glad")
set_target_properties(imgui PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(imguizmo PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(spdlog PROPERTIES FOLDER "Utilities")
set_target_properties(glfw PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(uninstall PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(update_mappings PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(yaml-cpp-parse PROPERTIES FOLDER "Utilities/yaml-cpp")
set_target_properties(yaml-cpp-read PROPERTIES FOLDER "Utilities/yaml-cpp")
set_target_properties(yaml-cpp-sandbox PROPERTIES FOLDER "Utilities/yaml-cpp")
#======================================
message(STATUS "Fetching thirdparty libraries done")
