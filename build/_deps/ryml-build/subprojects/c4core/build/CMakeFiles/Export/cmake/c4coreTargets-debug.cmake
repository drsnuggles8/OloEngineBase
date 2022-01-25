#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "c4core::c4core" for configuration "Debug"
set_property(TARGET c4core::c4core APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(c4core::c4core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/c4core.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS c4core::c4core )
list(APPEND _IMPORT_CHECK_FILES_FOR_c4core::c4core "${_IMPORT_PREFIX}/lib/c4core.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
