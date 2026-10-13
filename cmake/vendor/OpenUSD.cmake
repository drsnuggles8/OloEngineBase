# OpenUSD integration (#655) — default-ON USD import, matching UE5's USD-by-default posture.
#
# Static-monolithic OpenUSD is built from source via ExternalProject, ONCE, into a per-user cache
# keyed on the pinned USD tag and the oneTBB version it compiles against (so every worktree/clone
# on the machine reuses the same build). To make USD "just work" in ANY engine config, BOTH Debug
# and Release are built and installed into per-config prefixes (install/Debug, install/Release),
# and the engine links the one matching the CRT IT IS ACTUALLY COMPILED WITH — Debug engine ->
# Debug USD normally, but -> Release USD under ASan/fuzzing, which force the release CRT in every
# config (issue #1096; see the selector below). First build adds ~30-45 min + ~10 GB; later builds
# are cache hits.
#
# ONE oneTBB (#1575): USD compiles against vcpkg's oneTBB — the same package OpenVDB uses — and the
# engine links vcpkg's TBB::tbb. USD used to build its own oneTBB (2022.2) beside vcpkg's (2023.1),
# so every USD-enabled executable linked two static oneTBB runtimes of different versions, and the
# linker took whichever archive came first for every TBB symbol, including OpenVDB's.
# cmake/OneTbbGuard.cmake fails the configure if a second oneTBB archive reaches a link again.
#
# Exports (consumed by OloEngine/CMakeLists.txt), each possibly carrying a $<CONFIG> generator
# expression so the right per-config artifact is picked at build time:
#   OloEngine_USD_INCLUDE_DIR, OloEngine_USD_LIB (usd_m), OloEngine_USD_PLUGIN_TREE, the imported
#   target TBB::tbb (GLOBAL), and the target `olo_usd_ext` the engine add_dependencies() on.
# Turn the whole thing off with -DOLO_WITH_USD=OFF (recommended for CI that doesn't need USD).

include(ExternalProject)

# Pinned USD version (bump deliberately; a new SHA changes the cache key below so a stale build is
# never reused).
set(_OLO_USD_TAG "363a7c8da8d1937072a5f0989e91faf72eb1fa76")  # OpenUSD v25.11

# A prebuilt install carries the oneTBB it was built against, which is the second-runtime problem
# #1575 removed. Refuse it rather than ignore it: a user who set this expects it to be used.
if(OLO_USD_INSTALL_DIR)
    message(FATAL_ERROR
        "OLO_USD_INSTALL_DIR is no longer supported (#1575). A prebuilt USD install bundles its own "
        "oneTBB, and linking it beside vcpkg's oneTBB puts two oneTBB runtimes in one executable. "
        "Unset OLO_USD_INSTALL_DIR: USD now builds from source against vcpkg's oneTBB, once per "
        "machine, into the shared cache under $LOCALAPPDATA/OloEngine (or ~/.cache/oloengine).")
endif()

# vcpkg's oneTBB (the `usd` manifest feature in vcpkg.json lists `tbb`, so it is installed whenever
# OLO_WITH_USD is on). GLOBAL so the one imported target is visible to OloEngine-VolumeCook's
# OpenVDB lookup, which reuses it, and to the configure-time oneTBB guard at the top level.
find_package(TBB CONFIG REQUIRED GLOBAL)
if(NOT TARGET TBB::tbb)
    message(FATAL_ERROR "find_package(TBB) succeeded but defined no TBB::tbb target (TBB_DIR=${TBB_DIR})")
endif()

# Platform-appropriate static-library filename for the monolithic USD lib: usd_m.lib on Windows,
# libusd_m.a on Unix (PXR_BUILD_MONOLITHIC + BUILD_SHARED_LIBS=OFF).
set(_olo_usd_libfile "${CMAKE_STATIC_LIBRARY_PREFIX}usd_m${CMAKE_STATIC_LIBRARY_SUFFIX}")

# --- Build from source into a short, per-user, version-keyed cache (short path dodges Windows
#     MAX_PATH; git long-paths handles USD's deeply-nested source tree) ---
# The key names the full oneTBB version too: usd_m compiles oneTBB's headers into its objects, so
# any vcpkg oneTBB bump, a patch release included, must rebuild USD rather than reuse objects built
# against other headers.
string(SUBSTRING "${_OLO_USD_TAG}" 0 8 _olo_usd_key)
string(APPEND _olo_usd_key "-tbb${TBB_VERSION}")
if(WIN32)
    set(_olo_usd_cache "$ENV{LOCALAPPDATA}/OloEngine/usd-${_olo_usd_key}")
else()
    set(_olo_usd_cache "$ENV{HOME}/.cache/oloengine/usd-${_olo_usd_key}")
endif()
set(_olo_usd_install "${_olo_usd_cache}/install")

# Per-config layout: install/{Debug,Release}. The engine links the config whose CRT it is
# ACTUALLY compiled against — which is not the same question as `$<CONFIG:Debug>` (issue #1096).
#
# Sanitizers.cmake and Fuzzing.cmake force CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL in EVERY
# config, because the ASan/libFuzzer runtimes link against the release CRT. So an ASan Debug build
# compiles with /MD and _ITERATOR_DEBUG_LEVEL=0, while install/Debug/lib/usd_m.lib is a genuine
# debug-CRT build at level 2. Selecting it by config alone made `clangcl-asan --config Debug`
# compile all ~1595 TUs and then die at the link:
#
#   lld-link: error: /failifmismatch: mismatch detected for '_ITERATOR_DEBUG_LEVEL':
#   >>> ...McpFieldRegistry.cpp.obj has value 0
#   >>> usd_m.lib(pch.obj) has value 2
#
# /WHOLEARCHIVE:usd_m.lib is on the link line, so it cannot be dropped to sidestep this. The rule,
# stated so the next vendored dependency gets it right: pick a debug variant by the CRT in use,
# never by $<CONFIG:Debug> — the latter is only a PROXY for the former, and the sanitizer options
# break the correspondence.
if(OLO_ENABLE_ASAN OR OLO_ENABLE_FUZZING)
    set(_olo_usd_debug_crt "0")            # release CRT in every config, Debug included
else()
    set(_olo_usd_debug_crt "$<CONFIG:Debug>")
endif()
set(_olo_usd_cfg "$<IF:${_olo_usd_debug_crt},Debug,Release>")
file(MAKE_DIRECTORY
    "${_olo_usd_install}/Debug/include"   "${_olo_usd_install}/Debug/lib"
    "${_olo_usd_install}/Release/include" "${_olo_usd_install}/Release/lib")

set(OloEngine_USD_INCLUDE_DIR "${_olo_usd_install}/${_olo_usd_cfg}/include" CACHE INTERNAL "")
set(OloEngine_USD_LIB         "${_olo_usd_install}/${_olo_usd_cfg}/lib/${_olo_usd_libfile}" CACHE INTERNAL "")
# oneTBB itself is vcpkg's TBB::tbb. Its Debug configuration (tbb12_debug, debug CRT) follows the
# same effective-CRT rule without help here: Sanitizers.cmake and Fuzzing.cmake also set
# CMAKE_MAP_IMPORTED_CONFIG_DEBUG=Release, so a release-CRT Debug build links vcpkg's tbb12.
set(OloEngine_USD_PLUGIN_TREE "${_olo_usd_install}/${_olo_usd_cfg}/lib/usd" CACHE INTERNAL "")

# --- Cache hit: a COMPLETE install is already in the shared cache, so don't re-enter the build ---
#
# The cache holds the source/binary/install trees, but ExternalProject's STAMP_DIR defaults to
# the CONSUMING build tree. A second worktree therefore has no stamps, concludes every step is
# out of date, and re-runs configure+build inside the FIRST worktree's usd-build — which is not
# idempotent: BUILD_COMMAND is `--config Release` then `--config Debug` in one binary dir, so
# re-entering a finished tree hits `error C2859: ...arch.pdb is not the pdb file that was used
# when this precompiled header was created` across USD's arch target. The artifacts were shared
# but the "already done" record was not.
#
# Fixing it by moving STAMP_DIR into the cache would only relocate the problem (two worktrees
# configuring for the first time would then race on the stamps). Instead: if every artifact the
# engine actually consumes is present for BOTH configs, skip ExternalProject entirely and treat
# the cache as a prebuilt install — the second and every later worktree then never writes to the
# shared tree at all. Deliberately requires both configs and the plugInfo trees: a partial cache
# (interrupted first build) falls through and resumes the real build.
#
# Bumping _OLO_USD_TAG changes the cache key, so a version bump invalidates this automatically;
# to force a genuine rebuild, delete ${_olo_usd_cache}.
#
# Still NOT safe: two worktrees running their FIRST USD build concurrently — they share one
# binary dir. Same rule as the msvc/clangcl trees (docs/agent-rules/build-trees-and-windows-asan.md):
# sequence the first build, then every tree after it is a lock-free cache hit.
set(_olo_usd_cache_complete TRUE)
foreach(_olo_cfg Debug Release)
    if(NOT EXISTS "${_olo_usd_install}/${_olo_cfg}/lib/${_olo_usd_libfile}"
       OR NOT EXISTS "${_olo_usd_install}/${_olo_cfg}/include/pxr"
       OR NOT IS_DIRECTORY "${_olo_usd_install}/${_olo_cfg}/lib/usd")
        set(_olo_usd_cache_complete FALSE)
    endif()
endforeach()
if(_olo_usd_cache_complete)
    add_custom_target(olo_usd_ext)  # no-op; keeps the engine's add_dependencies() uniform
    message(STATUS "OLO_WITH_USD: reusing the complete OpenUSD cache at ${_olo_usd_install} "
                   "(Debug + Release) — no build step. Delete ${_olo_usd_cache} to force a rebuild.")
    return()
endif()

# Build+install BOTH configs. The VS generator is multi-config, so one configured tree compiles
# either config; the Release compile is reused incrementally across the two build steps.
set(_olo_usd_byproducts
    "${_olo_usd_install}/Release/lib/${_olo_usd_libfile}" "${_olo_usd_install}/Debug/lib/${_olo_usd_libfile}")

# Which toolchain builds USD. An ExternalProject inherits the generator but NOT the compilers.
#
# On Windows USD must be built by MSVC's cl. Under a Ninja tree with no MSVC environment, the
# nested configure searched PATH and found LLVM's GNU-style clang++, for which USD takes its
# non-MSVC path and stops at `'cxxabi.h' file not found` in arch/demangle.cpp. Handing it the
# parent's clang-cl does not work either: USD's compiler defaults treat any Clang as GNU-style and
# pass -Wno-deprecated, which clang-cl reads as `/wd`. So a Ninja (or any non-VS) parent builds USD
# with the Visual Studio generator, which locates the toolset itself. Every cache that ever built
# was seeded that way, by an msvc-preset tree; the other trees only ever hit the cache.
#
# Elsewhere the parent's compilers are passed through, so USD and the engine share one toolchain.
set(_olo_usd_generator_args "")
set(_olo_usd_toolchain_args "")
if(WIN32)
    # The msvc preset's generator by default; a box with another Visual Studio overrides it.
    set(OLO_USD_VS_GENERATOR "Visual Studio 18 2026" CACHE STRING
        "Visual Studio generator that builds OpenUSD when the engine tree is not a Visual Studio one")
    if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
        set(_olo_usd_generator_args CMAKE_GENERATOR "${OLO_USD_VS_GENERATOR}" CMAKE_GENERATOR_PLATFORM x64)
    endif()
else()
    foreach(_olo_var CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_LINKER CMAKE_AR)
        if(${_olo_var})
            list(APPEND _olo_usd_toolchain_args "-D${_olo_var}=${${_olo_var}}")
        endif()
    endforeach()
endif()

ExternalProject_Add(olo_openusd
    GIT_REPOSITORY  https://github.com/PixarAnimationStudios/OpenUSD.git
    GIT_TAG         ${_OLO_USD_TAG}
    GIT_SHALLOW     FALSE
    GIT_CONFIG      core.longpaths=true
    SOURCE_DIR      "${_olo_usd_cache}/usd-src"
    BINARY_DIR      "${_olo_usd_cache}/usd-build"
    ${_olo_usd_generator_args}
    # Guard the static-lib PDB-install bug before configure (see patch-usd-pdb.cmake), and make
    # the work library include oneTBB's version header itself, which vcpkg's oneTBB no longer
    # brings in transitively (see patch-usd-tbb-version.cmake).
    PATCH_COMMAND   ${CMAKE_COMMAND} -DUSD_SRC=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/patch-usd-pdb.cmake
            COMMAND ${CMAKE_COMMAND} -DUSD_SRC=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/patch-usd-tbb-version.cmake
    # Point USD at vcpkg's oneTBB (TBB_DIR is the config directory find_package() resolved above).
    # Its TBBConfig carries both configurations, so the Release and Debug USD builds each compile
    # against the oneTBB the engine links; only headers matter here, since usd_m is a static
    # archive that links nothing itself. The path is the vcpkg tree of whichever worktree built
    # the cache first; every other worktree's tree holds the same oneTBB version, because the
    # version is part of the cache key.
    CMAKE_ARGS
        # MUST be set at CONFIGURE time even though INSTALL_COMMAND passes --prefix.
        # `cmake --install --prefix` only redirects RELATIVE install destinations. USD's
        # top-level pxr/CMakeLists installs pxrConfig.cmake to an ABSOLUTE destination baked
        # from the configure-time CMAKE_INSTALL_PREFIX, so without this the generated
        # pxr/cmake_install.cmake ends with
        #     file(INSTALL DESTINATION "C:/Program Files (x86)/usd" ... pxrConfig.cmake)
        # and the install dies with "file cannot create directory ... Maybe need
        # administrative privileges" on any non-elevated shell.
        #
        # That failure is nastier than it looks: INSTALL_COMMAND runs Release THEN Debug, and
        # the Release failure aborts the whole step — so Debug never installs at all and the
        # engine link fails on a missing install/Debug/lib/usd_m.lib, pointing at a file
        # whose absence has nothing to do with the real error hundreds of log lines earlier.
        # It also only ever bites on a cache MISS, and USD is built once per machine, so it
        # can lie dormant indefinitely and then surface as "the build randomly broke".
        #
        # Release is an arbitrary but harmless choice of prefix for the stray absolute rule:
        # pxrConfig.cmake is a find_package() shim we never consume (the engine links
        # usd_m.lib by path), and every destination we DO consume is relative and therefore
        # correctly per-config via --prefix.
        -DCMAKE_INSTALL_PREFIX=${_olo_usd_install}/Release
        ${_olo_usd_toolchain_args}
        -DTBB_DIR=${TBB_DIR}
        -DPXR_FIND_TBB_IN_CONFIG=ON
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
    # Each configuration through build-usd-config.cmake, which picks the job count at build time:
    # build-lock.ps1's memory-aware CMAKE_BUILD_PARALLEL_LEVEL when it is set, else the physical
    # core count. A bare --parallel here ran the native tool's own default and ignored the lock.
    BUILD_COMMAND   ${CMAKE_COMMAND} -DUSD_BUILD_DIR=<BINARY_DIR> -DUSD_CONFIG=Release
                    -P ${CMAKE_CURRENT_LIST_DIR}/build-usd-config.cmake
            COMMAND ${CMAKE_COMMAND} -DUSD_BUILD_DIR=<BINARY_DIR> -DUSD_CONFIG=Debug
                    -P ${CMAKE_CURRENT_LIST_DIR}/build-usd-config.cmake
    INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Release --prefix ${_olo_usd_install}/Release
            COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config Debug --prefix ${_olo_usd_install}/Debug
    BUILD_BYPRODUCTS "${_olo_usd_byproducts}")

add_custom_target(olo_usd_ext DEPENDS olo_openusd)
message(STATUS "OLO_WITH_USD: OpenUSD build from source against vcpkg oneTBB ${TBB_VERSION} (Debug + Release) into ${_olo_usd_cache} "
               "(~30-45 min + ~10 GB on the FIRST build, then a machine-wide cache hit).")
