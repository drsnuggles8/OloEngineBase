# OloEngine vcpkg triplet — MSVC (cl.exe), static libraries against the DYNAMIC CRT.
#
# WHY NOT the commonly-cited `x64-windows-static`: that triplet uses the STATIC
# CRT. Three of this project's settings establish static libs against the
# *dynamic* CRT — `USE_STATIC_MSVC_RUNTIME_LIBRARY OFF` (Jolt),
# `protobuf_MSVC_STATIC_RUNTIME OFF`, `gtest_force_shared_crt ON` — and
# cmake/CommonProperties.cmake sets MSVC_RUNTIME_LIBRARY to
# `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` on every engine target. A CRT
# mismatch between a vcpkg-built static lib and the engine is heap corruption
# across a library boundary at RUNTIME, not a link error — silent, and far worse
# than a build failure. Do not "simplify" this to x64-windows-static.
#
# vcpkg ships a stock triplet with this exact name and linkage. We own a copy
# here (resolved first via VCPKG_OVERLAY_TRIPLETS) so the settings are versioned
# with the repo, reviewable in a diff, and extensible without forking vcpkg —
# and so the triplet content, which feeds every port's ABI hash, is identical
# byte-for-byte across worktrees. See docs/agent-rules/vcpkg-dependency-management.md.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
