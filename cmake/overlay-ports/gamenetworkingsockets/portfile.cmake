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
    # POST-RELEASE MASTER SNAPSHOT, on purpose. v1.6.0 (2026-06-03) is still the
    # latest tag, and it predates the fix for the stack-buffer-overflow that
    # GameNetworkingSockets_Init's service thread trips under AddressSanitizer:
    # ValveSoftware/GameNetworkingSockets#418, fixed by PR #420 (merged to master
    # 2026-08-24). Without that fix, NetworkManager::Init has to skip the live
    # init under ASan and the networking suites lose their sanitizer coverage.
    # Move back to a tag as soon as one ships that contains #420.
    REF "a424b7db649438acafb60c99cae6667587c42732" # master @ 2026-08-26, 17 commits after PR #420
    SHA512 a4a9abe49d1ee638926c180ff88c59329a3df749a6ba9a59efba4b719857f802941ae8eabb07fd88ea2138718a5721ecc1ebc61f2dafacc2a026e1374546e1b5
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
# Fixing this inside the port keeps every consumer from carrying a Findsodium.cmake
# shim (the exact class of in-tree plumbing issue #773 exists to delete).
#
# Define an INTERFACE target NAMED `sodium` rather than rewriting the link list.
# An earlier version of this port did the latter, matching the substring ";sodium;"
# — which is POSITION DEPENDENT and silently incomplete. On Windows the list reads
# "…;Threads::Threads;sodium;ws2_32;…" so it matched; on Linux there are no trailing
# platform libs, so an occurrence ends the list as `;sodium"` and did not.
# vcpkg_replace_string only requires ONE hit, so the port still built green and the
# failure landed on the CONSUMER's link line instead:
#
#   ld.lld: error: unable to find library -lsodium
#
# (CMake turns an unresolved bare target name into a raw -l flag.) An alias target
# resolves wherever `sodium` appears, so list ordering cannot defeat it.
vcpkg_replace_string(
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/GameNetworkingSocketsConfig.cmake"
    "find_dependency(sodium)"
    "find_dependency(unofficial-sodium)
    if(NOT TARGET sodium)
        add_library(sodium INTERFACE IMPORTED)
        set_target_properties(sodium PROPERTIES
            INTERFACE_LINK_LIBRARIES unofficial-sodium::sodium)
    endif()")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
