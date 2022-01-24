include(FetchContent)
set(FETCHCONTENT_QUIET off)

message(STATUS "Fetching third party libraries")

# Declare all the dependency details first in case any dependency wants
# to pull in some of the same ones (this keeps us in control)
FetchContent_Declare(
    entt
    URL https://github.com/skypjack/entt/archive/master.zip
    )
FetchContent_MakeAvailable(entt)
message(STATUS "${Green} entt Should Be Downloaded ${ColourReset}")
    
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v0.1.36
    )
    FetchContent_MakeAvailable(glad)
message(STATUS "${Green} glad Should Be Downloaded ${ColourReset}")

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

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)
message(STATUS "${Green} ImGui Should Be Downloaded ${ColourReset}")

FetchContent_Declare(ryml
    GIT_REPOSITORY https://github.com/biojppm/rapidyaml.git
    GIT_TAG master
    GIT_SHALLOW FALSE  # ensure submodules are checked out
)
FetchContent_MakeAvailable(ryml)
message(STATUS "${Green} ryml Should Be Downloaded ${ColourReset}")

FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.9.2
)
FetchContent_MakeAvailable(spdlog)
message(STATUS "${Green} spdlog Should Be Downloaded ${ColourReset}")

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
	${CMAKE_CURRENT_BINARY_DIR}/_deps/installs/${libStbImage}/include/${libStbImage}
)
set(STBIMAGE_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/installs/${libStbImage})
message(STATUS "${Green} stb_image Should Be Downloaded ${ColourReset}")

#======================================
message(STATUS "Fetching thirdparty libraries done")