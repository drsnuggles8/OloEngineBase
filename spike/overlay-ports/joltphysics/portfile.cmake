# OloEngine overlay port (issue #773/#774 vcpkg spike): the upstream vcpkg
# registry port hardcodes CROSS_PLATFORM_DETERMINISTIC=OFF as a plain option,
# not a feature, so it cannot be selected from a manifest. The engine requires
# it ON (deterministic replay / networking parity, #281 failure class). The
# only diff from the registry port is that one line below.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jrouwe/JoltPhysics
    REF "v${VERSION}"
    SHA512 bc6f2436bef91a6ffd09eee98186be645f6e9a9b3f65a7a645abf00432ac40ada03320c1fe946661817a94eda372cf8d88e4889991cdd25f1c16ecf9a4486677
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" USE_STATIC_CRT)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        debugrenderer       DEBUG_RENDERER_IN_DEBUG_AND_RELEASE
        profiler            PROFILER_IN_DEBUG_AND_RELEASE
        rtti                CPP_RTTI_ENABLED
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/Build"
    OPTIONS
        -DTARGET_UNIT_TESTS=OFF
        -DTARGET_HELLO_WORLD=OFF
        -DTARGET_PERFORMANCE_TEST=OFF
        -DTARGET_SAMPLES=OFF
        -DTARGET_VIEWER=OFF
        -DCROSS_PLATFORM_DETERMINISTIC=ON
        -DINTERPROCEDURAL_OPTIMIZATION=OFF
        -DUSE_STATIC_MSVC_RUNTIME_LIBRARY=${USE_STATIC_CRT}
        -DENABLE_ALL_WARNINGS=OFF
        -DOVERRIDE_CXX_FLAGS=OFF
        -DJPH_USE_DX12=OFF
        -DJPH_USE_VK=OFF
        -DJPH_USE_MTL=OFF
        ${FEATURE_OPTIONS}
    OPTIONS_RELEASE
        -DGENERATE_DEBUG_SYMBOLS=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_cmake_config_fixup(PACKAGE_NAME Jolt CONFIG_PATH "lib/cmake/Jolt")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
