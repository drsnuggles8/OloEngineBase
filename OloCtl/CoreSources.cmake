# The transport-free oloctl core, listed ONCE.
#
# Two targets compile these: the `oloctl` executable, and OloEngine-Tests — which
# pulls them in by path so the tests can drive the REAL RunCli against a bare
# AutomationRegistry, the same way it pulls in the editor's MCP dispatch core.
#
# Included by both rather than duplicated, because the failure mode of a
# duplicated list is silent: a new core source added to only one of them still
# builds, and the half that lacks it just stops covering whatever moved into it.
#
# CMAKE_CURRENT_LIST_DIR (not CMAKE_CURRENT_SOURCE_DIR) so the paths resolve
# against THIS file's directory no matter which directory includes it.
#
# McpHttpCommandSource.cpp is deliberately absent: it is the one translation unit
# that needs a socket, and it belongs to the executable alone.
set(OLOCTL_CORE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/OloCtl/ArgumentBinder.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/OloCtl/CliRunner.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/OloCtl/CommandCatalogue.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/OloCtl/CommandTree.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/OloCtl/HelpText.cpp
)

set(OLOCTL_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
