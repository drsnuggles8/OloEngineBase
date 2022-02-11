#############################################
#            Install                        #
#############################################

configure_file("${CMAKE_CURRENT_LIST_DIR}/findDependancies.cmake" "findDependancies.cmake" COPYONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/preamble.cmake" "preamble.cmake" COPYONLY)

# Make cache variables for install destinations
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Generate the config file that is includes the exports
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/Config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/projTempConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/projTemp
  NO_SET_AND_CHECK_MACRO
  NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

if(NOT DEFINED projTemp_VERSION_MAJOR)
    message("\n\n\n\n warning, projTemp_VERSION_MAJOR not defined ${projTemp_VERSION_MAJOR}")
endif()

set_property(TARGET projTemp PROPERTY VERSION ${projTemp_VERSION})

# Generate the version file for the config file
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/projTempConfigVersion.cmake"
  VERSION "${projTemp_VERSION_MAJOR}.${projTemp_VERSION_MINOR}.${projTemp_VERSION_PATCH}"
  COMPATIBILITY AnyNewerVersion
)

# Install the configuration file
install(FILES
          "${CMAKE_CURRENT_BINARY_DIR}/projTempConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/projTempConfigVersion.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/findDependancies.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/preamble.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/projTemp
)

# Install library
install(
    TARGETS projTemp
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    EXPORT projTempTargets)

# Install headers
install(
    DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../projTemp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/"
    FILES_MATCHING PATTERN "*.h")

# install config and use the "namespace" of pt::
install(EXPORT projTempTargets
  FILE projTempTargets.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/projTemp
       NAMESPACE pt::
)
 export(EXPORT projTempTargets
       FILE "${CMAKE_CURRENT_BINARY_DIR}/projTempTargets.cmake"
       NAMESPACE pt::
)