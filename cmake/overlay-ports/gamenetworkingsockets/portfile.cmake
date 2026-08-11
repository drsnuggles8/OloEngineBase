# OloEngine overlay port (issue #773). Three deltas from the registry port:
#
#   1. USE_CRYPTO / USE_CRYPTO25519 = libsodium, not OpenSSL. The registry port
#      hardcodes `-DUSE_CRYPTO=OpenSSL` and takes an `openssl` dependency. This
#      engine has always used libsodium for GNS and vendors it for exactly that
#      purpose; taking a whole TLS stack into the supply chain to avoid owning
#      a portfile is a bad trade.
#   2. `ice` is NOT a default feature (vcpkg.json). The engine builds GNS with
#      ENABLE_ICE=OFF — no P2P/STUN/TURN — and vcpkg's default-features would
#      silently turn it back on.
#   3. GNS's bundled cmake/Findsodium.cmake is REPLACED wholesale (see the header
#      of the Findsodium.cmake next to this file for why). Short version: it
#      detects the target architecture by regex-scraping compiler diagnostics,
#      which yields "unknown" under clang-cl and hard-fails the whole triplet, and
#      it searches a vendor-SDK directory layout a vcpkg install does not have.
#      The replacement just re-exports vcpkg's `unofficial-sodium::sodium` under
#      the `sodium` name GNS links against.
#
# When refreshing this overlay after a vcpkg baseline bump, re-diff against
# $VCPKG_ROOT/ports/gamenetworkingsockets/portfile.cmake.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ValveSoftware/GameNetworkingSockets
    REF "2cb93a06350bb065db53abdb0d87cf297e0bfd34" # v1.6.0
    SHA512 c2deaa3aab42cd840dd13560ca4da40faa375ab846ea15af38d55eb7acc48cfe8cbdbe0c76b9c3484d26f9e1163e36ac1eb73a317e5c19cefe60d0b861d19e06
    HEAD_REF master
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        ice             ENABLE_ICE
)

# Select static vs dynamic based on the triplet.
if("${VCPKG_LIBRARY_LINKAGE}" STREQUAL "dynamic")
    set(BUILD_SHARED_LIB ON)
    set(BUILD_STATIC_LIB OFF)
else()
    set(BUILD_SHARED_LIB OFF)
    set(BUILD_STATIC_LIB ON)
endif()

# Link the MSVC CRT statically when the CRT linkage is static.
# Not used on non-MSVC platforms; listed in MAYBE_UNUSED_VARIABLES accordingly.
if("${VCPKG_CRT_LINKAGE}" STREQUAL "static")
    set(MSVC_CRT_STATIC ON)
else()
    set(MSVC_CRT_STATIC OFF)
endif()

# Delta 3: swap in a vcpkg-aware Findsodium.cmake before configuring.
file(COPY "${CMAKE_CURRENT_LIST_DIR}/Findsodium.cmake"
     DESTINATION "${SOURCE_PATH}/cmake")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUSE_CRYPTO=libsodium
        -DUSE_CRYPTO25519=libsodium
        -DBUILD_STATIC_LIB=${BUILD_STATIC_LIB}
        -DBUILD_SHARED_LIB=${BUILD_SHARED_LIB}
        -DMSVC_CRT_STATIC=${MSVC_CRT_STATIC}
        -DBUILD_TESTS=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_TOOLS=OFF
        -DProtobuf_USE_STATIC_LIBS=ON
        ${FEATURE_OPTIONS}
    MAYBE_UNUSED_VARIABLES
        MSVC_CRT_STATIC
        Protobuf_USE_STATIC_LIBS
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/GameNetworkingSockets")
vcpkg_fixup_pkgconfig()

# --- Delta 4: bridge GNS's exported "sodium" package/target to vcpkg's naming ---
# Building with USE_CRYPTO=libsodium bakes two references to a package and target
# literally named `sodium` into the installed config:
#   GameNetworkingSocketsConfig.cmake -> find_dependency(sodium)
#   GameNetworkingSockets.cmake       -> INTERFACE_LINK_LIBRARIES "...;sodium;..."
# vcpkg's libsodium port publishes neither: it exports the package
# `unofficial-sodium` with the target `unofficial-sodium::sodium`. Left alone, the
# CONSUMER's find_package(GameNetworkingSockets) fails at configure with
# "Could not find a package configuration file provided by sodium" — the port
# itself builds and installs perfectly, so this only surfaces downstream.
#
# Rewriting the two references here keeps the fix inside the port that caused it,
# rather than making every consumer carry a Findsodium.cmake shim (the exact class
# of in-tree plumbing issue #773 exists to delete). vcpkg_replace_string errors if
# a pattern stops matching, so an upstream rename fails loudly at port build.
vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/GameNetworkingSocketsConfig.cmake"
    "find_dependency(sodium)"
    "find_dependency(unofficial-sodium)")
vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/GameNetworkingSockets.cmake"
    ";sodium;"
    ";unofficial-sodium::sodium;")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
