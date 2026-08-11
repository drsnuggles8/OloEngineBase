# OloEngine custom triplet (issue #773/#774 vcpkg spike).
#
# Static libraries against the DYNAMIC CRT. This is deliberately NOT the
# community "x64-windows-static" triplet (static CRT) — three of our build
# settings require the dynamic CRT (USE_STATIC_MSVC_RUNTIME_LIBRARY OFF,
# protobuf_MSVC_STATIC_RUNTIME OFF, gtest_force_shared_crt ON). A CRT mismatch
# between a vcpkg-built static lib and the engine is heap corruption across a
# library boundary, not a link error — silent and much worse than a build
# failure. See docs/agent-rules/build-trees-and-windows-asan.md region for CRT
# background.
#
# vcpkg actually ships this exact triplet already (scripts/../triplets/
# x64-windows-static-md.cmake) — we own a copy here rather than relying on the
# stock one so the full #773 migration can extend it (e.g. per-toolset
# overrides) without forking vcpkg's copy, and so this spike proves the
# "custom triplet" mechanism end-to-end, not just "the triplet vcpkg already
# has happens to be right."
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
