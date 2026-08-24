project(OloEngine-VolumeCook
        VERSION 0.0.1
        LANGUAGES CXX)

# OpenVDB (.vdb) volumetric import (#724), confined to its own static lib so it never
# reaches the shipped runtime (OloRuntime/OloServer). See the OLO_WITH_OPENVDB option
# comment in the root CMakeLists.txt and docs/agent-rules/asset-import-usd-alembic.md
# for the general "editor/cook-only dependency" shape this follows (mirrors how
# httplib::httplib is linked onto OloEditor only, not OloEngine).
#
# OpenVDB's vcpkg port pulls in blosc, boost (several components), imath, openexr and
# tbb — all of that stays out of OloEngine, OloRuntime and OloServer entirely; only
# OloEditor and OloEngine-Tests (for CPU-side contract-test coverage of the grid ->
# texture transform math) link this target.
find_package(OpenVDB CONFIG REQUIRED)
# OpenVDB's own CMake package does not re-export blosc as a transitive link
# requirement of OpenVDB::openvdb (a vcpkg port target-name mismatch of the
# kind docs/agent-rules/vcpkg-dependency-management.md warns about — "the
# overlay was found" / "the package was found" is not "the option/link
# reached the consumer"): the archive references blosc_compress_ctx et al.
# but nothing propagates the symbols, so linking OpenVDB::openvdb alone
# fails with `undefined symbol: blosc_compress_ctx` at the final .exe link,
# not at OpenVDB::openvdb's own link (a static archive doesn't resolve
# symbols against other archives). Link blosc explicitly.
find_package(blosc CONFIG REQUIRED)

# FindOpenVDB.cmake (shipped by the openvdb vcpkg port; `find_package(OpenVDB
# CONFIG REQUIRED)` is silently redirected to it by the port's vcpkg-cmake-
# wrapper.cmake — there is no OpenVDBConfig.cmake) is NOT multi-config aware:
# it resolves OpenVDB::openvdb's IMPORTED_LOCATION with a single, non-suffixed
# find_library() call rather than per-config IMPORTED_LOCATION_DEBUG/RELEASE
# properties. Under the Ninja Multi-Config generator this silently binds ONE
# config's .lib regardless of which config a given `--config` build actually
# links — verified by reproducing locally: a Release OloEditor build picked up
# `debug/lib/libopenvdb.lib` (a real link failure, `_ITERATOR_DEBUG_LEVEL`
# mismatch: OloEngineRenderer.lib has 0, libopenvdb.lib has 2) even though the
# SAME generator resolves every other vcpkg dependency (all proper CONFIG
# packages with real per-config properties) correctly. This is the same class
# of bug as vcpkg-dependency-management.md's Trap 4 (a mismatched CRT/debug
# runtime that is a HEAP-CORRUPTION-shaped failure at runtime if it links at
# all — here it doesn't even link, which is the safe direction to fail in),
# via a different mechanism than that trap documents. TBB::tbb is pulled in
# the same way (FindOpenVDB.cmake's `find_package(TBB CONFIG REQUIRED)` runs
# inside the same non-config-aware resolution context) and needs the same fix.
#
# Fix: point both imported targets at BOTH configs explicitly, derived from
# vcpkg's own install layout (`<triplet>/lib` vs `<triplet>/debug/lib`) rather
# than trusting either Find-module's single guess. See
# docs/agent-rules/asset-import-openvdb-volumetric.md for the full story.
# release_lib_name / debug_lib_name are separate args: most vcpkg ports keep
# the same filename in `lib/` vs `debug/lib/` (e.g. libopenvdb.lib), but some
# (TBB: `tbb12.lib` vs `tbb12_debug.lib`) genuinely rename it per config.
function(olo_fix_vcpkg_multiconfig_imported_location target_name release_lib_name debug_lib_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "olo_fix_vcpkg_multiconfig_imported_location: no such target ${target_name}")
    endif()
    set(_release_lib "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/${release_lib_name}")
    set(_debug_lib "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib/${debug_lib_name}")
    if(NOT EXISTS "${_release_lib}" OR NOT EXISTS "${_debug_lib}")
        message(FATAL_ERROR "olo_fix_vcpkg_multiconfig_imported_location: expected both\n"
            "  ${_release_lib}\n  ${_debug_lib}\nto exist for ${target_name} — vcpkg install layout changed?")
    endif()
    set_target_properties(${target_name} PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release"
        IMPORTED_LOCATION_DEBUG "${_debug_lib}"
        IMPORTED_LOCATION_RELEASE "${_release_lib}"
    )
endfunction()

olo_fix_vcpkg_multiconfig_imported_location(OpenVDB::openvdb libopenvdb.lib libopenvdb.lib)
olo_fix_vcpkg_multiconfig_imported_location(TBB::tbb tbb12.lib tbb12_debug.lib)

add_subdirectory(src)

olo_set_compiler_options(OloEngine-VolumeCook)
olo_set_common_definitions(OloEngine-VolumeCook)

# Needs OloEngine's public headers (Ref<T>, Base.h typedefs, glm, Log) but must NOT
# pull OloEngine's own compile definitions/link requirements onto callers beyond that —
# PUBLIC here matches how OloEngine itself links its other asset-import translators.
# PUBLIC (not PRIVATE): VolumeImportTest.cpp (OloEngine-Tests) authors synthetic
# .vdb fixtures directly against the OpenVDB API to pin the grid -> texture
# transform math against openvdb::math::Transform::indexToWorld() itself — it
# needs OpenVDB's headers on its own include path, not just OpenVDBVolumeCook's.
target_link_libraries(OloEngine-VolumeCook PUBLIC OloEngine)
target_link_libraries(OloEngine-VolumeCook PUBLIC OpenVDB::openvdb blosc_static)

target_compile_definitions(OloEngine-VolumeCook PUBLIC OLO_WITH_OPENVDB=1)

target_include_directories(OloEngine-VolumeCook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
