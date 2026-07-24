# OpenUSD integration (#655) — default-ON USD import, matching UE5's USD-by-default posture.
#
# Two ways to get USD, chosen automatically:
#   1. If OLO_USD_INSTALL_DIR points at a prebuilt static-monolithic USD + oneTBB install, use it
#      directly (fast path — CI caches, a shared team build, or a locally-built prefix). This is
#      a FLAT single-config install; the consumer is responsible for CRT/config matching.
#   2. Otherwise build oneTBB + static-monolithic OpenUSD from source via ExternalProject, ONCE,
#      into a per-user cache keyed on the pinned versions (so every worktree/clone on the machine
#      reuses the same build). To make USD "just work" in ANY engine config, BOTH Debug and Release
#      are built and installed into per-config prefixes (install/Debug, install/Release), and the
#      engine links the CRT-matching one (Debug engine -> Debug USD; Release/RelWithDebInfo/MinSizeRel
#      -> Release USD). First build adds ~30-45 min + ~10 GB; later builds are cache hits.
#
# Exports (consumed by OloEngine/CMakeLists.txt), each possibly carrying a $<CONFIG> generator
# expression so the right per-config artifact is picked at build time:
#   OloEngine_USD_INCLUDE_DIR, OloEngine_USD_LIB_DIR, OloEngine_USD_LIB (usd_m), OloEngine_USD_TBB_LIB,
#   OloEngine_USD_PLUGIN_TREE, and the target `olo_usd_ext` the engine add_dependencies() on.
# Turn the whole thing off with -DOLO_WITH_USD=OFF (recommended for CI that doesn't need USD).

include(ExternalProject)

# Pinned versions (bump deliberately; a new SHA changes the cache key below so a stale build is
# never reused).
set(_OLO_USD_TAG "363a7c8da8d1937072a5f0989e91faf72eb1fa76")  # OpenUSD v25.11
set(_OLO_TBB_TAG "06ce6212da6710f4bb2d20a1904b018aa44069bf")  # oneTBB v2022.2.0

if(OLO_USD_INSTALL_DIR AND EXISTS "${OLO_USD_INSTALL_DIR}/include/pxr")
    set(OloEngine_USD_INCLUDE_DIR "${OLO_USD_INSTALL_DIR}/include"          CACHE INTERNAL "")
    set(OloEngine_USD_LIB_DIR     "${OLO_USD_INSTALL_DIR}/lib"              CACHE INTERNAL "")
    set(OloEngine_USD_LIB         "${OLO_USD_INSTALL_DIR}/lib/usd_m.lib"    CACHE INTERNAL "")
    set(OloEngine_USD_TBB_LIB     "${OLO_USD_INSTALL_DIR}/lib/tbb12.lib"    CACHE INTERNAL "")
    set(OloEngine_USD_PLUGIN_TREE "${OLO_USD_INSTALL_DIR}/lib/usd"          CACHE INTERNAL "")
    add_custom_target(olo_usd_ext)  # no-op; keeps the engine's add_dependencies() uniform
    message(STATUS "OLO_WITH_USD: using prebuilt OpenUSD install at ${OLO_USD_INSTALL_DIR} "
                   "(flat single-config — ensure it matches your engine build's CRT/config)")
    return()
endif()

# --- Build from source into a short, per-user, version-keyed cache (short path dodges Windows
#     MAX_PATH; git long-paths handles the deeply-nested oneTBB doc files) ---
string(SUBSTRING "${_OLO_USD_TAG}" 0 8 _olo_usd_key)
if(WIN32)
    set(_olo_usd_cache "$ENV{LOCALAPPDATA}/OloEngine/usd-${_olo_usd_key}")
else()
    set(_olo_usd_cache "$ENV{HOME}/.cache/oloengine/usd-${_olo_usd_key}")
endif()
set(_olo_usd_install "${_olo_usd_cache}/install")

# Per-config layout: install/{Debug,Release}. The engine links the CRT-matching config
# (Debug -> Debug; everything else uses the release CRT -> Release).
set(_olo_usd_cfg "$<IF:$<CONFIG:Debug>,Debug,Release>")
file(MAKE_DIRECTORY
    "${_olo_usd_install}/Debug/include"   "${_olo_usd_install}/Debug/lib"
    "${_olo_usd_install}/Release/include" "${_olo_usd_install}/Release/lib")

set(OloEngine_USD_INCLUDE_DIR "${_olo_usd_install}/${_olo_usd_cfg}/include" CACHE INTERNAL "")
set(OloEngine_USD_LIB_DIR     "${_olo_usd_install}/${_olo_usd_cfg}/lib"     CACHE INTERNAL "")
set(OloEngine_USD_LIB         "${_olo_usd_install}/${_olo_usd_cfg}/lib/usd_m.lib" CACHE INTERNAL "")
# oneTBB names its Debug import lib tbb12_debug.lib; Release is tbb12.lib.
set(OloEngine_USD_TBB_LIB
    "$<IF:$<CONFIG:Debug>,${_olo_usd_install}/Debug/lib/tbb12_debug.lib,${_olo_usd_install}/Release/lib/tbb12.lib>"
    CACHE INTERNAL "")
set(OloEngine_USD_PLUGIN_TREE "${_olo_usd_install}/${_olo_usd_cfg}/lib/usd" CACHE INTERNAL "")

# Build+install BOTH configs. The VS generator is multi-config, so one configured tree compiles
# either config; the Release compile is reused incrementally across the two build steps.
set(_olo_tbb_byproducts
    "${_olo_usd_install}/Release/lib/tbb12.lib" "${_olo_usd_install}/Debug/lib/tbb12_debug.lib")
set(_olo_usd_byproducts
    "${_olo_usd_install}/Release/lib/usd_m.lib" "${_olo_usd_install}/Debug/lib/usd_m.lib")

ExternalProject_Add(olo_onetbb
    GIT_REPOSITORY  https://github.com/uxlfoundation/oneTBB.git
    GIT_TAG         ${_OLO_TBB_TAG}
    GIT_SHALLOW     FALSE
    GIT_CONFIG      core.longpaths=true
    SOURCE_DIR      "${_olo_usd_cache}/tbb-src"
    BINARY_DIR      "${_olo_usd_cache}/tbb-build"
    CMAKE_ARGS
        -DBUILD_SHARED_LIBS=OFF -DTBB_TEST=OFF -DTBBMALLOC_BUILD=ON -DTBB_STRICT=OFF
        # Disable oneTBB's internal Debug assertions (TBB_USE_ASSERT gates __TBB_ASSERT). A static
        # Debug TBB otherwise trips a benign worker-teardown assert (small_object_pool.cpp:143,
        # "m_private_counter >= 0") that aborts the process at exit — cosmetic (all work completes)
        # but it would pop an assert dialog on every Debug editor close. Off in Release already.
        "-DCMAKE_CXX_FLAGS=-DTBB_USE_ASSERT=0"
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel
            COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Debug --parallel
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release --prefix ${_olo_usd_install}/Release
            COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Debug --prefix ${_olo_usd_install}/Debug
    BUILD_BYPRODUCTS "${_olo_tbb_byproducts}")

ExternalProject_Add(olo_openusd
    DEPENDS         olo_onetbb
    GIT_REPOSITORY  https://github.com/PixarAnimationStudios/OpenUSD.git
    GIT_TAG         ${_OLO_USD_TAG}
    GIT_SHALLOW     FALSE
    GIT_CONFIG      core.longpaths=true
    SOURCE_DIR      "${_olo_usd_cache}/usd-src"
    BINARY_DIR      "${_olo_usd_cache}/usd-build"
    # Guard the static-lib PDB-install bug before configure (see patch-usd-pdb.cmake).
    PATCH_COMMAND   ${CMAKE_COMMAND} -DUSD_SRC=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/patch-usd-pdb.cmake
    # Point USD at oneTBB via CMAKE_PREFIX_PATH covering both per-config installs.
    CMAKE_ARGS
        -DCMAKE_PREFIX_PATH=${_olo_usd_install}/Release$<SEMICOLON>${_olo_usd_install}/Debug
        -DTBB_DIR=${_olo_usd_install}/Release/lib/cmake/TBB
        -DBUILD_SHARED_LIBS=OFF
        -DPXR_BUILD_MONOLITHIC=ON
        -DPXR_ENABLE_PYTHON_SUPPORT=OFF
        -DPXR_BUILD_IMAGING=OFF
        -DPXR_BUILD_USD_IMAGING=OFF
        -DPXR_BUILD_USDVIEW=OFF
        -DPXR_BUILD_TESTS=OFF
        -DPXR_BUILD_EXAMPLES=OFF
        -DPXR_BUILD_TUTORIALS=OFF
        -DPXR_BUILD_DOCUMENTATION=OFF
        -DPXR_BUILD_USD_TOOLS=OFF
        -DPXR_ENABLE_GL_SUPPORT=OFF
        -DPXR_ENABLE_MATERIALX_SUPPORT=OFF
        -DPXR_ENABLE_OPENVDB_SUPPORT=OFF
        -DPXR_ENABLE_OSL_SUPPORT=OFF
        -DPXR_ENABLE_PTEX_SUPPORT=OFF
        -DPXR_ENABLE_HDF5_SUPPORT=OFF
        -DPXR_PREFER_SAFETY_OVER_SPEED=ON
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --parallel
            COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Debug --parallel
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release --prefix ${_olo_usd_install}/Release
            COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Debug --prefix ${_olo_usd_install}/Debug
    BUILD_BYPRODUCTS "${_olo_usd_byproducts}")

add_custom_target(olo_usd_ext DEPENDS olo_openusd)
message(STATUS "OLO_WITH_USD: OpenUSD + oneTBB build from source (Debug + Release) into ${_olo_usd_cache} "
               "(~30-45 min + ~10 GB on the FIRST build, then a machine-wide cache hit). "
               "Pass -DOLO_USD_INSTALL_DIR=<prefix> to reuse a prebuilt install.")
