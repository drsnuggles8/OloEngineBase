#----------------------------------------------------------------
# Generated CMake target import file for configuration "Dist".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "glfw" for configuration "Dist"
set_property(TARGET glfw APPEND PROPERTY IMPORTED_CONFIGURATIONS DIST)
set_target_properties(glfw PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DIST "C"
  IMPORTED_LOCATION_DIST "${_IMPORT_PREFIX}/lib/glfw3.lib"
  )

list(APPEND _IMPORT_CHECK_TARGETS glfw )
list(APPEND _IMPORT_CHECK_FILES_FOR_glfw "${_IMPORT_PREFIX}/lib/glfw3.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
