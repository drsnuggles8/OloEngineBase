# Build one configuration of OpenUSD's ExternalProject tree (its BUILD_COMMAND):
#   cmake -DUSD_BUILD_DIR=<binary dir> -DUSD_CONFIG=<Release|Debug> -P build-usd-config.cmake
#
# The job count is decided HERE, at build time, because that is when the environment that sets it
# exists: build-lock.ps1's memory-aware CMAKE_BUILD_PARALLEL_LEVEL, else the physical core count.
# A bare --parallel would be the native tool's own default (every core for MSBuild, cores + 2 for
# Ninja) and ignore the lock; no flag at all builds MSBuild's projects one at a time.
#
# Under a Visual Studio generator the count goes to /MP, not to MSBuild's /m. USD compiles every
# project with /MP (msvcdefaults.cmake), so /m:N would run N projects each starting a compiler per
# core. One project at a time with CL_MPCount=N (the /MP count MSBuild passes to cl) keeps the total
# at N.

# A -P script starts with every policy unset; pin them, as OneTbbGuard.cmake does.
cmake_minimum_required(VERSION 3.27)

foreach(_var USD_BUILD_DIR USD_CONFIG)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "build-usd-config: ${_var} is not set")
    endif()
endforeach()

set(_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
if(NOT _jobs MATCHES "^[1-9][0-9]*$")
    cmake_host_system_information(RESULT _jobs QUERY NUMBER_OF_PHYSICAL_CORES)
endif()
if(NOT _jobs MATCHES "^[1-9][0-9]*$")
    cmake_host_system_information(RESULT _jobs QUERY NUMBER_OF_LOGICAL_CORES)
endif()
if(NOT _jobs MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "build-usd-config: no usable job count (CMAKE_BUILD_PARALLEL_LEVEL unset and "
                        "the host reports no cores); set CMAKE_BUILD_PARALLEL_LEVEL")
endif()

if(NOT EXISTS "${USD_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "build-usd-config: ${USD_BUILD_DIR}/CMakeCache.txt not found; is the USD tree configured?")
endif()
file(STRINGS "${USD_BUILD_DIR}/CMakeCache.txt" _generator REGEX "^CMAKE_GENERATOR:INTERNAL=")
if(NOT _generator)
    message(FATAL_ERROR "build-usd-config: no CMAKE_GENERATOR in ${USD_BUILD_DIR}/CMakeCache.txt, "
                        "so the job count cannot be routed to the right flag")
endif()
if(_generator MATCHES "=Visual Studio")
    # /nodeReuse:false: reused MSBuild nodes outlive the build holding handles inside the shared
    # per-machine cache, which then cannot be moved or deleted until they time out.
    set(_parallel --parallel 1 -- "/p:CL_MPCount=${_jobs}" /nodeReuse:false)
    message(STATUS "build-usd-config: building OpenUSD ${USD_CONFIG}, one project at a time with /MP${_jobs}")
else()
    set(_parallel --parallel ${_jobs})
    message(STATUS "build-usd-config: building OpenUSD ${USD_CONFIG} with ${_jobs} jobs")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${USD_BUILD_DIR}" --config "${USD_CONFIG}" ${_parallel}
    RESULT_VARIABLE _result)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "build-usd-config: the OpenUSD ${USD_CONFIG} build failed (${_result})")
endif()
