include(FetchContent)
set(FETCHCONTENT_QUIET off)
set(FETCHCONTENT_BASE_DIR ${PROJECT_SOURCE_DIR}/vendor CACHE PATH "Missing description." FORCE)

# Set CMake policies to suppress warnings in vendor libraries
if(POLICY CMP0194)  
    cmake_policy(SET CMP0194 NEW)  # MSVC assembler policy for libpng
endif()

message(STATUS "Downloading third party libraries using CMake's FetchContent and CPM")

FetchContent_Declare(
	assimp
	GIT_REPOSITORY	https://github.com/assimp/assimp.git
	GIT_TAG			master
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	box2d
	GIT_REPOSITORY	https://github.com/erincatto/box2d.git
	GIT_TAG			v3.0.0
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	entt
	GIT_REPOSITORY	https://github.com/skypjack/entt.git
	GIT_TAG			master
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	filewatch
	GIT_REPOSITORY	https://github.com/drsnuggles8/filewatch.git
	GIT_TAG			master
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	freetype
	GIT_REPOSITORY	https://github.com/freetype/freetype.git
	GIT_TAG			master
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	glad
	GIT_REPOSITORY	https://github.com/Dav1dde/glad.git
	GIT_TAG			v2.0.8
	GIT_SHALLOW		TRUE
	SOURCE_SUBDIR	cmake
)
FetchContent_Declare(
	glfw
	GIT_REPOSITORY	https://github.com/glfw/glfw.git
	GIT_TAG			3.4
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	glm
	GIT_REPOSITORY https://github.com/g-truc/glm.git
	GIT_TAG master
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG main
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	joltphysics
	GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
	GIT_TAG v5.3.0
	GIT_SHALLOW		TRUE
	SOURCE_SUBDIR Build
)
FetchContent_Declare(
    libpng
    GIT_REPOSITORY https://github.com/glennrp/libpng.git
    GIT_TAG v1.6.46
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	miniaudio
	GIT_REPOSITORY https://github.com/mackron/miniaudio.git
	GIT_TAG master
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	msdfgen
	GIT_REPOSITORY https://github.com/Chlumsky/msdfgen.git
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	msdf-atlas-gen
	GIT_REPOSITORY https://github.com/Chlumsky/msdf-atlas-gen.git
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	spdlog
	GIT_REPOSITORY	https://github.com/gabime/spdlog.git
	GIT_TAG			v1.15.1
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	tracy
	GIT_REPOSITORY	https://github.com/wolfpld/tracy.git
	GIT_TAG			v0.11.1
	GIT_SHALLOW		TRUE
	GIT_PROGRESS    TRUE
)
FetchContent_Declare(
    tinyxml2
    GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
    GIT_TAG 10.0.0
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
	yaml-cpp
	GIT_REPOSITORY	https://github.com/jbeder/yaml-cpp.git
	GIT_TAG			master
	GIT_SHALLOW		TRUE
)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
	GIT_SHALLOW		TRUE
)

FetchContent_GetProperties(zlib)
FetchContent_MakeAvailable(zlib)
set(ZLIB_LIBRARY_DEBUG "${zlib_BINARY_DIR}/Debug/${CMAKE_STATIC_LIBRARY_PREFIX}zlibd${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(ZLIB_LIBRARY_RELEASE "${zlib_BINARY_DIR}/Release/${CMAKE_STATIC_LIBRARY_PREFIX}zlib${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(ZLIB_LIBRARY_DIST "${zlib_BINARY_DIR}/Dist/${CMAKE_STATIC_LIBRARY_PREFIX}zlib${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(ZLIB_LIBRARY $<$<CONFIG:Debug>:${ZLIB_LIBRARY_DEBUG}>$<$<CONFIG:Release>:${ZLIB_LIBRARY_RELEASE}>$<$<CONFIG:Dist>:${ZLIB_LIBRARY_DIST}>)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
set(ZLIB_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/zlib" "${CMAKE_CURRENT_BINARY_DIR}/zlib")
find_package(ZLIB REQUIRED)

FetchContent_GetProperties(libpng)
set(PNG_STATIC OFF CACHE BOOL "Build libpng as a static lib" FORCE)
set(PNG_SHARED ON CACHE BOOL "Build libpng as a shared lib" FORCE)
option(PNG_TOOLS OFF)
option(PNG_TESTS OFF)
option(PNG_DEBUG ON)
set(PNG_ARM_NEON "off" CACHE STRING "Disable ARM NEON optimizations" FORCE)
set(PNG_INTEL_SSE "off" CACHE STRING "Disable Intel SSE optimizations" FORCE)
FetchContent_MakeAvailable(libpng)
set(PNG_LIBRARY png CACHE INTERNAL "")
set(PNG_PNG_INCLUDE_DIR ${libpng_SOURCE_DIR} CACHE INTERNAL "")
set(PNG_LIBRARY_DIST ${libpng_BINARY_DIR}/Dist/libpng16.lib)
set(PNG_LIBRARY_RELEASE ${libpng_BINARY_DIR}/Release/libpng16.lib)
set(PNG_LIBRARY_DEBUG ${libpng_BINARY_DIR}/Debug/libpng16d.lib)
find_package(PNG REQUIRED)
if(TARGET PNG::PNG)
  set_target_properties(PNG::PNG PROPERTIES
    IMPORTED_LOCATION_DEBUG ${PNG_LIBRARY_DEBUG}
    IMPORTED_LOCATION_RELEASE ${PNG_LIBRARY_RELEASE}
    IMPORTED_LOCATION_DIST ${PNG_LIBRARY_DIST}
  )
endif()

# --- Fix: Force png_shared to use correct zlib for all configs ---
if(TARGET png_shared)
    # Remove all previous zlib dependencies
    set_property(TARGET png_shared PROPERTY INTERFACE_LINK_LIBRARIES "")
    set_property(TARGET png_shared PROPERTY LINK_LIBRARIES "")
    # Now add only the correct one for each config
    target_link_libraries(png_shared PRIVATE
        $<$<CONFIG:Debug>:${ZLIB_LIBRARY_DEBUG}>
        $<$<CONFIG:Release>:${ZLIB_LIBRARY_RELEASE}>
        $<$<CONFIG:Dist>:${ZLIB_LIBRARY_DIST}>
    )
    # Add zlib include directories so zlib.h can be found
    target_include_directories(png_shared PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
endif()

include_directories(${libpng_SOURCE_DIR} ${libpng_BINARY_DIR})
add_dependencies(png_shared zlib)

set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(freetype tinyxml2)
add_library(Freetype::Freetype ALIAS freetype)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE INTERNAL "" FORCE)
set(ASSIMP_BUILD_ZLIB OFF CACHE INTERNAL "" FORCE)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES Off CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS Off CACHE BOOL "" FORCE)
set(GLFW_INSTALL Off CACHE BOOL "" FORCE)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BuildTests OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA OFF CACHE INTERNAL "")
set(MSDFGEN_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
set(MSDF_ATLAS_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_USE_VCPKG OFF CACHE INTERNAL "")
set(MSDF_ATLAS_USE_SKIA OFF CACHE INTERNAL "")
set(MSDF_ATLAS_MSDFGEN_EXTERNAL ON CACHE BOOL "" FORCE)
set(MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(TRACY_ENABLE OFF CACHE BOOL "Enable profiling")
option(TRACY_ENABLE "Enable profiling" ON)
set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(assimp box2d entt filewatch glad glfw glm googletest joltphysics miniaudio msdfgen msdf-atlas-gen spdlog tracy yaml-cpp)

target_compile_definitions("TracyClient" PRIVATE $<$<CONFIG:Release>:TRACY_ENABLE>)

glad_add_library(glad STATIC API gl:core=4.6 LOCATION ${PROJECT_SOURCE_DIR}/vendor/glad-build/${TARGET})

FetchContent_Declare(stb_image
	GIT_REPOSITORY https://github.com/nothings/stb.git
	GIT_TAG master)
FetchContent_MakeAvailable(stb_image)
add_library(stb_image INTERFACE)
file(COPY ${stb_image_SOURCE_DIR}/stb_image.h DESTINATION "${PROJECT_SOURCE_DIR}/vendor/stb_image-build/include/stb_image")
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
  VERSION 5.4.7
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

set_target_properties(assimp PROPERTIES FOLDER "Utilities/assimp")
set_target_properties(UpdateAssimpLibsDebugSymbolsAndDLLs PROPERTIES FOLDER "Utilities/assimp")
set_target_properties(box2d PROPERTIES FOLDER "Utilities/box2d")
set_target_properties(simde PROPERTIES FOLDER "Utilities/box2d")
set_target_properties(glad PROPERTIES FOLDER "Utilities/glad")
set_target_properties(glfw PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(glm PROPERTIES FOLDER "Utilities")
set_target_properties(update_mappings PROPERTIES FOLDER "Utilities/GLFW3")
set_target_properties(gmock PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gmock_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(gtest_main PROPERTIES FOLDER "Utilities/googletest")
set_target_properties(Jolt PROPERTIES FOLDER "Utilities/JoltPhysics")
set_target_properties(miniaudio PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_channel_combiner_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_channel_separator_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_ltrim_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_reverb_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(miniaudio_vocoder_node PROPERTIES FOLDER "Utilities/miniaudio")
set_target_properties(imgui PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(imguizmo PROPERTIES FOLDER "Utilities/imgui")
set_target_properties(lua PROPERTIES FOLDER "Utilities/Lua")
set_target_properties(spdlog PROPERTIES FOLDER "Utilities")
set_target_properties(TracyClient PROPERTIES FOLDER "Utilities")
set_target_properties(yaml-cpp PROPERTIES FOLDER "Utilities")
set_target_properties(freetype PROPERTIES FOLDER "Utilities")
set_target_properties(tinyxml2 PROPERTIES FOLDER "Utilities")
set_target_properties(msdf-atlas-gen PROPERTIES FOLDER "Utilities/msdf-atlas-gen")
set_target_properties(msdfgen-core PROPERTIES FOLDER "Utilities/msdf-gen")
set_target_properties(msdfgen-ext PROPERTIES FOLDER "Utilities/msdf-gen")
set_target_properties(png_shared PROPERTIES FOLDER "Utilities/libpng")
set_target_properties(png_genfiles PROPERTIES FOLDER "Utilities/libpng")
set_target_properties(example PROPERTIES FOLDER "Utilities/zlib")
set_target_properties(minigzip PROPERTIES FOLDER "Utilities/zlib")
set_target_properties(zlib PROPERTIES FOLDER "Utilities/zlib")
set_target_properties(zlibstatic PROPERTIES FOLDER "Utilities/zlib")
#======================================
message(STATUS "Fetching thirdparty libraries done")
