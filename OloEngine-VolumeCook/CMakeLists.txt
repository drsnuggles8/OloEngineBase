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
