project(OloEditor
		VERSION 0.0.1
		LANGUAGES CXX)

# Main targets built by this project
add_subdirectory(src)

# Configure the OloEditor target using our common properties
olo_configure_app(OloEditor)

# Set up common include directories
olo_set_common_include_directories(OloEditor)

# OloEditor sources include each other with `Preview/...`, `Panels/...` etc.
# paths rooted at OloEditor/src — make that work regardless of where the
# including file lives in the tree.
target_include_directories(OloEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# target_link_libraries implies a build-order dependency on OloEngine, so no
# separate add_dependencies(... OloEngine) is needed.
# httplib::httplib is the header-only cpp-httplib package (vcpkg manifest, resolved in
# OloEngine/vendor/CMakeLists.txt); only OloEditor's MCP module (#285) uses it, so link
# it here rather than on OloEngine to avoid putting <winsock2.h> on the whole engine's
# include path.
target_link_libraries(OloEditor
                    OloEngine
                    httplib::httplib
)

# OpenVDB (.vdb) volumetric import (#724) — editor/cook-only, same shape as
# httplib above: the dependency and its consuming importer code are confined
# to OloEditor (via OloEngine-VolumeCook), never linked into OloEngine,
# OloRuntime or OloServer. See the OLO_WITH_OPENVDB option comment in the
# root CMakeLists.txt.
if(OLO_WITH_OPENVDB)
    target_link_libraries(OloEditor OloEngine-VolumeCook)
endif()

# Suppress MSVC's "use the _s variant" CRT warnings (C4996 on strncpy/sscanf/etc.).
# OloEngine already adopts the same stance for the engine library; matching here
# keeps the editor consistent and avoids per-call-site noise across the panels.
target_compile_definitions(OloEditor PRIVATE _CRT_SECURE_NO_WARNINGS)

# GLFW_INCLUDE_NONE stops <GLFW/glfw3.h> pulling in the system <GL/gl.h>, which
# defines __gl_h_ and makes a later <glad/gl.h> hard-error with C1189
# ("OpenGL (gl.h) header already included"). EditorLayer.cpp includes GLFW near
# the top and glad further down, so without this the order alone decides whether
# the editor compiles.
#
# OloEngine sets the same define, but PRIVATE, so it never reached this target.
# It went unnoticed because glad used to arrive first anyway, transitively via
# OloEnginePCH.h -> RendererAPI.h; issue #691 Phase 2 made RendererAPI.h GL-free
# and removed that accident. Declared per-target to match _CRT_SECURE_NO_WARNINGS
# above, and set on the whole target rather than fixing one include order so the
# next file to combine GLFW with glad cannot reintroduce it.
target_compile_definitions(OloEditor PRIVATE GLFW_INCLUDE_NONE)
