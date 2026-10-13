# Build one configuration of OpenUSD's ExternalProject tree (its BUILD_COMMAND):
#   cmake -DUSD_BUILD_DIR=<binary dir> -DUSD_CONFIG=<Release|Debug> -P build-usd-config.cmake
#
# The job count is decided HERE, at build time, because that is when the environment that sets it
# exists. build-lock.ps1 exports CMAKE_BUILD_PARALLEL_LEVEL with its memory-aware count, and it is
# passed on as --parallel. Without it (a build outside the lock, or from the Visual Studio IDE) the
# physical core count is used. A bare --parallel would be the native tool's own default (every core
# for MSBuild, cores + 2 for Ninja), which ignores the lock, and no flag at all builds MSBuild's
# projects one at a time.

foreach(_var USD_BUILD_DIR USD_CONFIG)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "build-usd-config: ${_var} is not set")
    endif()
endforeach()

set(_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
if(NOT _jobs MATCHES "^[1-9][0-9]*$")
    cmake_host_system_information(RESULT _jobs QUERY NUMBER_OF_PHYSICAL_CORES)
endif()
message(STATUS "build-usd-config: building OpenUSD ${USD_CONFIG} with ${_jobs} jobs")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${USD_BUILD_DIR}" --config "${USD_CONFIG}" --parallel ${_jobs}
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "build-usd-config: the OpenUSD ${USD_CONFIG} build failed (${_result})")
endif()
