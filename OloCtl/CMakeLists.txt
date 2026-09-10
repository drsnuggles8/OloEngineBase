project(oloctl
        VERSION 0.1.0
        LANGUAGES CXX)

# oloctl — a command-line frontend over the Automation Registry (issue #1125).
#
# WHERE IT SITS IN THE TARGET GRAPH: nowhere near the engine. It links neither
# OloEngine nor OloEditor, and it has no edge to GenerateBindings, because it
# compiles no component header and holds no schema knowledge of its own — it
# learns the whole command surface at runtime from a running editor. Its entire
# dependency set is nlohmann/json and cpp-httplib, both header-only.
#
# That is a design constraint, not an accident of the moment. Linking the engine
# here would put a GL/Vulkan/physics build behind a JSON-over-HTTP CLI, and —
# worse — would make it possible for oloctl to answer a question from its own
# compiled-in knowledge instead of from the editor it is supposed to be
# reporting on. The one place the two trees meet is
# OloCtl/src/OloCtl/RegistryCommandSource.h, which is header-only and compiled
# only by OloEngine-Tests (see OloEngine/tests/CMakeLists.txt).
#
# The transport-free sources are ALSO compiled into OloEngine-Tests, the same way
# the editor's MCP dispatch core is. Both read the list from CoreSources.cmake
# rather than each keeping its own copy — see that file for why.
include(${CMAKE_CURRENT_SOURCE_DIR}/CoreSources.cmake)

add_executable(oloctl
    ${OLOCTL_CORE_SOURCES}
    # The only translation unit that includes httplib.
    src/OloCtl/McpHttpCommandSource.cpp
    src/main.cpp
)

olo_configure_app(oloctl)

# Deliberately NOT olo_set_common_include_directories(): that puts OloEngine/src
# and the vendor tree on the include path, and the point of this target is that
# it cannot reach either.
target_include_directories(oloctl PRIVATE
    ${OLOCTL_INCLUDE_DIR}
)

target_link_libraries(oloctl PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
)

# Suppress MSVC's "use the _s variant" CRT warnings, matching OloEditor and the
# engine. strtoll/strtod in the argument binder are the call sites.
target_compile_definitions(oloctl PRIVATE _CRT_SECURE_NO_WARNINGS)

# FOLDER is assigned at the root CMakeLists.txt to keep app folder grouping in
# one place.
