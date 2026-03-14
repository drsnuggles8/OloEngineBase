# Custom FindSodium.cmake — redirects to FetchContent-built libsodium target.
# Placed in cmake/ which is on CMAKE_MODULE_PATH, so this is found before
# GNS's bundled FindSodium.cmake.

if(TARGET sodium)
    set(sodium_FOUND TRUE)
    # GNS expects these variables from FindSodium
    if(NOT sodium_INCLUDE_DIR)
        get_target_property(sodium_INCLUDE_DIR sodium INTERFACE_INCLUDE_DIRECTORIES)
    endif()
    set(sodium_LIBRARY_RELEASE sodium)
    set(sodium_LIBRARY_DEBUG sodium)
    return()
endif()

# Fall back to system search
find_path(sodium_INCLUDE_DIR sodium.h PATH_SUFFIXES sodium)
find_library(sodium_LIBRARY_RELEASE NAMES sodium libsodium)
find_library(sodium_LIBRARY_DEBUG NAMES sodiumd libsodiumd sodium libsodium)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(sodium
    REQUIRED_VARS sodium_LIBRARY_RELEASE sodium_INCLUDE_DIR)

if(sodium_FOUND AND NOT TARGET sodium)
    add_library(sodium UNKNOWN IMPORTED)
    set_target_properties(sodium PROPERTIES
        IMPORTED_LOCATION "${sodium_LIBRARY_RELEASE}"
        INTERFACE_INCLUDE_DIRECTORIES "${sodium_INCLUDE_DIR}")
endif()
