# OloEngine overlay (issue #773): drop-in replacement for GameNetworkingSockets'
# bundled cmake/Findsodium.cmake, copied over the original by portfile.cmake.
#
# The bundled module is written for a hand-installed libsodium SDK and is wrong for
# a vcpkg tree in two independent ways:
#
#   1. Its Windows/MSVC branch derives the target architecture by compiling a file
#      full of `#error ARCH_VALUE …` directives and regex-scraping the compiler's
#      diagnostics. Under clang-cl the diagnostic text does not match what the regex
#      expects, so _TARGET_ARCH comes out empty and the module hard-fails:
#          the unknown architecture is not supported by Findsodium.cmake.
#      That is what broke the x64-windows-static-md-clangcl triplet.
#   2. Even when the arch check passes, it then searches a vendor-SDK layout
#      (`<root>/x64/<Config>/v143/static/`) that a vcpkg install does not have, and
#      declares BOTH the debug and release library paths as REQUIRED_VARS — so a
#      half-successful search is a hard failure rather than a fallback.
#
# vcpkg already publishes libsodium as the `unofficial-sodium` package with a
# properly config-aware `unofficial-sodium::sodium` imported target, so the whole
# search is unnecessary: just re-export it under the name GNS expects.
#
# GNS uses exactly two things from this module: a `sodium` target to link, and
# sodium_FOUND. (Its installed CMake config also emits `find_dependency(sodium)` and
# a literal `sodium` link entry — portfile.cmake rewrites both to the unofficial-
# sodium spelling so downstream consumers do not need this module at all.)

include(CMakeFindDependencyMacro)
find_dependency(unofficial-sodium CONFIG)

if(unofficial-sodium_FOUND AND NOT TARGET sodium)
    add_library(sodium INTERFACE IMPORTED)
    set_target_properties(sodium PROPERTIES
        INTERFACE_LINK_LIBRARIES "unofficial-sodium::sodium")
endif()

get_target_property(sodium_INCLUDE_DIR unofficial-sodium::sodium INTERFACE_INCLUDE_DIRECTORIES)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(sodium
    REQUIRED_VARS sodium_INCLUDE_DIR)
