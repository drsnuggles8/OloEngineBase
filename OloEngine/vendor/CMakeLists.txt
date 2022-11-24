include(FetchContent)
set(FETCHCONTENT_QUIET off)
set(FETCHCONTENT_BASE_DIR ${PROJECT_SOURCE_DIR}/vendor CACHE PATH "Missing description." FORCE)

message(STATUS "Downloading third party libraries using CMake's FetchContent and CPM")

FetchContent_Declare(assimp
	GIT_REPOSITORY https://github.com/assimp/assimp.git
	GIT_TAG master)
FetchContent_Declare(box2d
	GIT_REPOSITORY https://github.com/erincatto/box2d.git
	GIT_TAG main)
FetchContent_Declare(entt
	GIT_REPOSITORY https://github.com/skypjack/entt.git
	GIT_TAG master)
FetchContent_Declare(filewatch
	GIT_REPOSITORY https://github.com/ThomasMonkman/filewatch.git
	GIT_TAG master)
FetchContent_Declare(glad
	GIT_REPOSITORY https://github.com/Dav1dde/glad.git
	GIT_TAG v2.0.2
	SOURCE_SUBDIR cmake)
FetchContent_Declare(glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
	GIT_TAG 3.3.8)
FetchContent_Declare(glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG 0.9.9.8)
FetchContent_Declare(googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG main)
# Don't update spdlog to 1.11.0, gives errors in OpenGLContext.cpp
FetchContent_Declare(spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG v1.10.0)
FetchContent_Declare(yaml-cpp
	GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
	GIT_TAG master)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES Off CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS Off CACHE BOOL "" FORCE)
set(GLFW_INSTALL Off CACHE BOOL "" FORCE)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BuildTests OFF CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS Off CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS Off CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS Off CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(assimp box2d entt filewatch glad glfw glm googletest spdlog yaml-cpp)

glad_add_library(glad STATIC API gl:core=4.6 LOCATION ${PROJECT_SOURCE_DIR}/vendor/glad-build/${TARGET})

FetchContent_Declare(stb_image
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG master
	CONFIGURE_COMMAND ""
	BUILD_COMMAND "")
FetchContent_MakeAvailable(stb_image)
add_library(stb_image INTERFACE)
file(COPY
	${stb_image_SOURCE_DIR}/stb_image.h
DESTINATION
	"${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image"
)
set(STBIMAGE_INSTALL_DIR "${PROJECT_SOURCE_DIR}/vendor/stb_image-build")

CPMAddPackage(NAME imgui
	GITHUB_REPOSITORY ocornut/imgui
	GIT_TAG docking
	DOWNLOAD_ONLY YES)
if(imgui_ADDED)
	FILE(GLOB imgui_sources ${imgui_SOURCE_DIR}/*.cpp)
	file(GLOB IMGUI_HEADERS ${IMGUI_PATH}/*.h)
	FILE(GLOB imgui_sources_cpp ${imgui_SOURCE_DIR}/misc/cpp/*.cpp)

	set(IMGUI_DIR ${imgui_SOURCE_DIR})
	set(IMGUI_INCLUDE_DIR ${imgui_SOURCE_DIR})
	set(IMGUI_SOURCES ${imgui_sources} ${imgui_sources_cpp})

	add_library(imgui STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS} "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp" "${IMGUI_DIR}/backends/imgui_impl_opengl3.h" "${IMGUI_DIR}/backends/imgui_impl_opengl3_loader.h")
	target_compile_definitions(imgui PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD=1)
	target_include_directories(imgui PUBLIC ${IMGUI_DIR} "glfw-src/include" "glad-build/include")
	set_target_properties(imgui PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imgui-build")
endif()

CPMAddPackage(NAME imguizmo
	GITHUB_REPOSITORY CedricGuillemet/ImGuizmo
	GIT_TAG master
	DOWNLOAD_ONLY YES)
if (imguizmo_ADDED)
	file(GLOB IMGUIZMO_SOURCES ${imguizmo_SOURCE_DIR}/*.cpp)
	file(GLOB IMGUIZMO_HEADERS ${imguizmo_SOURCE_DIR}/*.h)  
	add_library(imguizmo STATIC ${IMGUIZMO_SOURCES} ${IMGUIZMO_HEADERS})
	target_include_directories(imguizmo PUBLIC ${imgui_SOURCE_DIR})
	set_target_properties(imguizmo	PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/imguizmo-build")
endif()

CPMAddPackage(NAME lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  VERSION 5.4.4
  DOWNLOAD_ONLY YES)  
if(lua_ADDED)
  file(GLOB lua_sources ${lua_SOURCE_DIR}/*.c)
  list(REMOVE_ITEM lua_sources "${lua_SOURCE_DIR}/lua.c" "${lua_SOURCE_DIR}/luac.c" "${lua_SOURCE_DIR}/onelua.c" "${lua_SOURCE_DIR}/ltests.c")
  add_library(lua STATIC ${lua_sources})
  target_include_directories(lua SYSTEM PUBLIC $<BUILD_INTERFACE:${lua_SOURCE_DIR}>)
  set_target_properties(lua	PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build"
		LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build"
		RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/vendor/lua-build")
endif()

set_target_properties(glad PROPERTIES FOLDER "Utilities/glad")
set_target_properties(imgui PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(imguizmo PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(spdlog PROPERTIES FOLDER "Utilities")
set_target_properties(glfw PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(uninstall PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(update_mappings PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(yaml-cpp PROPERTIES FOLDER "Utilities/yaml-cpp")
set_target_properties(gmock PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gmock_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(box2d PROPERTIES FOLDER "Utilities/box2d")
set_target_properties(lua PROPERTIES FOLDER "Utilities/Lua")
set_target_properties(assimp PROPERTIES FOLDER "Utilities/assimp")
set_target_properties(zlibstatic PROPERTIES FOLDER "Utilities/assimp")
set_target_properties(UpdateAssimpLibsDebugSymbolsAndDLLs PROPERTIES FOLDER "Utilities/assimp")
#======================================
message(STATUS "Fetching thirdparty libraries done")
