include(FetchContent)
set(FETCHCONTENT_QUIET off)
set(FETCHCONTENT_BASE_DIR ${PROJECT_SOURCE_DIR}/vendor CACHE PATH "Missing description." FORCE)

message(STATUS "Downloading third party libraries using CMake's FetchContent and CPM")

# entt - entity component system
FetchContent_Declare(
	entt
	GIT_REPOSITORY https://github.com/skypjack/entt.git
	GIT_TAG master
	)
FetchContent_MakeAvailable(entt)

# glad - GL/GLES/EGL/GLX/WGL Loader-Generator
FetchContent_Declare(
	glad
	GIT_REPOSITORY https://github.com/Dav1dde/glad.git
	GIT_TAG v0.1.36
	)
set(GLAD_PROFILE "core"	CACHE STRING "OpenGL profile")
set(GLAD_GENERATOR "c"	CACHE STRING "Language to generate the binding for")
FetchContent_MakeAvailable(glad)

# glfw - platform-independent API for window & context management
FetchContent_Declare(
	glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# glm - header-only mathematics library 
FetchContent_Declare(
	glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG 0.9.9.8
)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm)

# imgui - gui library for C++ - doesn't work with CMake, so we do our own thing
CPMAddPackage(
	NAME imgui
	GITHUB_REPOSITORY ocornut/imgui
	GIT_TAG docking
	DOWNLOAD_ONLY YES
	)
if (imgui_ADDED)
	FILE(GLOB imgui_sources ${imgui_SOURCE_DIR}/*.cpp)
	file(GLOB IMGUI_HEADERS ${IMGUI_PATH}/*.h)
	FILE(GLOB imgui_sources_cpp ${imgui_SOURCE_DIR}/misc/cpp/*.cpp)

	set(IMGUI_DIR ${imgui_SOURCE_DIR})
	set(IMGUI_INCLUDE_DIR ${imgui_SOURCE_DIR})
	set(IMGUI_SOURCES ${imgui_sources} ${imgui_sources_cpp})

	add_library("imgui" STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS} "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp" "${IMGUI_DIR}/backends/imgui_impl_opengl3.h" "${IMGUI_DIR}/backends/imgui_impl_opengl3_loader.h")
	target_compile_definitions("imgui" PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD=1)
	target_include_directories("imgui" PUBLIC ${IMGUI_DIR} "glfw-src/include" "glad-build/include")
	set_target_properties("imgui"
		PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
	)
endif()

# imguizmo - guizmo extension for C++ - doesn't work with CMake, so we do our own thing
CPMAddPackage(
	NAME imguizmo
	GITHUB_REPOSITORY CedricGuillemet/ImGuizmo
	GIT_TAG master
	DOWNLOAD_ONLY YES
	)
if (imguizmo_ADDED)
	file(GLOB IMGUIZMO_SOURCES ${imguizmo_SOURCE_DIR}/*.cpp)
	file(GLOB IMGUIZMO_HEADERS ${imguizmo_SOURCE_DIR}/*.h)  
	add_library("imguizmo" STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
	target_include_directories("imguizmo" PUBLIC ${imgui_SOURCE_DIR})
	set_target_properties(imguizmo
		PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
	)
endif()

# yaml-cpp - a YAML parser and emitter
FetchContent_Declare(yaml-cpp
	GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
	GIT_TAG master
)
set(YAML_CPP_BUILD_TESTS OFF)
FetchContent_MakeAvailable(yaml-cpp)

# spdlog - logging library
FetchContent_Declare(spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG v1.10.0
)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# stb_image - image loader
FetchContent_Declare(stb_image
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG master
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
)
FetchContent_MakeAvailable(stb_image)
add_library(stb_image INTERFACE)
file(COPY
	${stb_image_SOURCE_DIR}/stb_image.h
DESTINATION
	"${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image"
)
set(STBIMAGE_INSTALL_DIR "${PROJECT_SOURCE_DIR}/vendor/stb_image-build")

# Google Test - unit test library
FetchContent_Declare(googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG main
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Box 2D - 2d physics library
FetchContent_Declare(box2d
	GIT_REPOSITORY https://github.com/erincatto/box2d.git
	GIT_TAG main
)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(box2d)

# Lua - scripting language
CPMAddPackage(
  NAME lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  VERSION 5.4.4
  DOWNLOAD_ONLY YES
)
if(lua_ADDED)
  file(GLOB lua_sources ${lua_SOURCE_DIR}/*.c)
  list(REMOVE_ITEM lua_sources "${lua_SOURCE_DIR}/lua.c" "${lua_SOURCE_DIR}/luac.c" "${lua_SOURCE_DIR}/onelua.c" "${lua_SOURCE_DIR}/ltests.c")
  add_library(lua STATIC ${lua_sources})
  target_include_directories(lua SYSTEM PUBLIC $<BUILD_INTERFACE:${lua_SOURCE_DIR}>)
  set_target_properties(lua
		PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build"
	)
endif()

set_target_properties(glad PROPERTIES FOLDER "Utilities/glad")
set_target_properties(glad-generate-files PROPERTIES FOLDER "Utilities/glad")
set_target_properties(imgui PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(imguizmo PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(spdlog PROPERTIES FOLDER "Utilities")
set_target_properties(glfw PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(uninstall PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(update_mappings PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(yaml-cpp PROPERTIES FOLDER "Utilities/yaml-cpp")
set_target_properties(yaml-cpp-parse PROPERTIES FOLDER "Utilities/yaml-cpp")
set_target_properties(yaml-cpp-read PROPERTIES FOLDER "Utilities/yaml-cpp")
set_target_properties(yaml-cpp-sandbox PROPERTIES FOLDER "Utilities/yaml-cpp")
set_target_properties(gmock PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gmock_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(box2d PROPERTIES FOLDER "Utilities/box2d")
set_target_properties(lua PROPERTIES FOLDER "Utilities/Lua")
#======================================
message(STATUS "Fetching thirdparty libraries done")
