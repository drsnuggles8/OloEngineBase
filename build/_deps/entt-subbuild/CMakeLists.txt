# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.22.1)

# We name the project and the target for the ExternalProject_Add() call
# to something that will highlight to the user what we are working on if
# something goes wrong and an error message is produced.

project(entt-populate NONE)



include(ExternalProject)
ExternalProject_Add(entt-populate
                     "UPDATE_DISCONNECTED" "False" "URL" "https://github.com/skypjack/entt/archive/master.zip"
                    SOURCE_DIR          "C:/Users/ole/source/repos/OloEngine/build/_deps/entt-src"
                    BINARY_DIR          "C:/Users/ole/source/repos/OloEngine/build/_deps/entt-build"
                    CONFIGURE_COMMAND   ""
                    BUILD_COMMAND       ""
                    INSTALL_COMMAND     ""
                    TEST_COMMAND        ""
                    USES_TERMINAL_DOWNLOAD  YES
                    USES_TERMINAL_UPDATE    YES
)


