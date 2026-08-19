# Steamworks SDK path resolution (#644), gated on OLO_WITH_STEAM (root CMakeLists).
#
# WHY THIS LIVES AT THE ROOT AND NOT IN OloEngine/CMakeLists.txt (issue #828).
#
# These are plain variables consumed by SUBDIRECTORIES — OLO_STEAM_RUNTIME_DLL in particular is
# read by olo_copy_steam_runtime() (cmake/CommonProperties.cmake) from four different directories:
# OloEngine/tests, OloEditor/src, OloRuntime/src and OloServer/src. A CMake variable is only
# visible to a subdirectory added AFTER it is set, so resolving these anywhere other than before
# the first add_subdirectory() makes correctness depend on directory-processing order.
#
# That is exactly the bug this file was extracted to kill. The block used to sit at the bottom of
# OloEngine/CMakeLists.txt, which processes add_subdirectory(tests) near its top — so on a FRESH
# configure OLO_STEAM_RUNTIME_DLL was still undefined when OloEngine-Tests asked for it, the copy
# step was silently skipped, and the test executable shipped without steam_api64.dll. It then died
# at gtest discovery with 0xC0000135 (STATUS_DLL_NOT_FOUND) — an error naming nothing about Steam.
# The second configure "fixed" it only because the CACHE INTERNAL write from the first run was
# still there, which is why it read as "works on my machine".
#
# So: keep this include ABOVE every add_subdirectory() in the root CMakeLists. Everything here is
# pure path resolution and validation — no targets are touched, deliberately, so it has no
# ordering requirement of its own. The target wiring (include dirs, the import library, the
# compile definitions) stays in OloEngine/CMakeLists.txt where the OloEngine target exists.
#
# The API surface comes from exactly one of two mutually exclusive places:
#   * the REAL SDK, resolved from $ENV{STEAMWORKS_SDK_ROOT}. Never vendored: the licence forbids
#     redistribution and this repo is public, so developers install it themselves (it is free —
#     see docs/ops/build.md). This mirrors how the Vulkan SDK is found, NOT how Mono is vendored
#     in-tree — Mono may live in-tree only because it is redistributable.
#   * our hand-written STUBS (OLO_WITH_STEAM_STUB_SDK), which let CI compile, link and RUN the
#     ON path that the real SDK can never reach. See the root CMakeLists for why that matters.
#     That path resolves no external paths at all, so it does nothing here.
if(OLO_WITH_STEAM AND NOT OLO_WITH_STEAM_STUB_SDK)
	# Both halves matter. WIN32 is TRUE for 32-bit Windows too, and everything below hard-codes
	# the x64 redistributables (steam_api64.lib / steam_api64.dll) — so a 32-bit configure would
	# sail past a WIN32-only guard and fail much later at link, with an error that says nothing
	# about pointer size. Reject it here, where the message can.
	if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
		message(FATAL_ERROR
			"OLO_WITH_STEAM=ON is only wired for 64-bit Windows, the stated target for issue #644 "
			"(this configure: WIN32=${WIN32}, pointer size=${CMAKE_SIZEOF_VOID_P} bytes). "
			"The paths below use the SDK's win64 redistributables exclusively. The SDK does ship "
			"win32/linux64/osx variants, so extending this is a small change — but none of it is "
			"tested here, and silently disabling would hide that. "
			"Use -DOLO_WITH_STEAM=OFF, or -DOLO_WITH_STEAM_STUB_SDK=ON to compile the Steam path "
			"without any SDK at all.")
	endif()

	if(NOT DEFINED ENV{STEAMWORKS_SDK_ROOT})
		message(FATAL_ERROR
			"OLO_WITH_STEAM=ON but STEAMWORKS_SDK_ROOT is not set. Download the Steamworks SDK "
			"(free, needs only an ordinary Steam account — the $100 Steam Direct fee is for "
			"publishing, not building) and point the variable at the directory containing "
			"public/ and redistributable_bin/. Full instructions: docs/ops/build.md.")
	endif()

	# Point at the inner `sdk` directory — the one DIRECTLY containing public/ and
	# redistributable_bin/. Pointing one level out is the obvious mistake and would otherwise
	# surface as an unhelpful "cannot open include file: steam/steam_api.h" much later.
	set(OLO_STEAM_SDK_ROOT "$ENV{STEAMWORKS_SDK_ROOT}")

	# The include dir is the SDK ROOT, NOT root/public — and the one TU that uses it includes
	# "public/steam/steam_api.h" rather than "steam/steam_api.h".
	#
	# THIS IS NOT A STYLE CHOICE. Adding …/public would put a directory with a `steam/` child on
	# the engine include path, and every `#include <steam/…>` in the NETWORKING code would then
	# resolve against Valve's SDK instead of GameNetworkingSockets'. The Steamworks SDK ships its
	# own isteamnetworkingutils.h / steamnetworkingtypes.h / isteamnetworkingsockets.h /
	# steam_api_common.h / steamtypes.h, so the netcode ends up with Valve's
	# <steam/isteamnetworkingutils.h> (which Valve HAS) alongside GNS's
	# <steam/steamnetworkingsockets.h> (which Valve does NOT) — half of each SDK in one TU, dying
	# with 12 × "C2365: 'k_iSteamNetworkingSocketsCallbacks': redefinition" reported inside a
	# Steamworks header from Networking files that never mention Steam.
	#
	# Rooting one level higher makes the hijack impossible by construction. See
	# docs/agent-rules/steamworks-platform-integration.md.
	set(OLO_STEAM_INCLUDE_DIR "${OLO_STEAM_SDK_ROOT}")
	set(OLO_STEAM_IMPORT_LIB  "${OLO_STEAM_SDK_ROOT}/redistributable_bin/win64/steam_api64.lib")
	set(OLO_STEAM_RUNTIME_DLL "${OLO_STEAM_SDK_ROOT}/redistributable_bin/win64/steam_api64.dll")

	foreach(_olo_steam_path
			"${OLO_STEAM_SDK_ROOT}/public/steam/steam_api.h"
			"${OLO_STEAM_IMPORT_LIB}"
			"${OLO_STEAM_RUNTIME_DLL}")
		if(NOT EXISTS "${_olo_steam_path}")
			message(FATAL_ERROR
				"STEAMWORKS_SDK_ROOT=${OLO_STEAM_SDK_ROOT} does not look like a Steamworks SDK: "
				"missing ${_olo_steam_path}. It must point at the directory DIRECTLY containing "
				"public/ and redistributable_bin/ (i.e. the inner 'sdk' folder of the zip), not "
				"its parent. See docs/ops/build.md.")
		endif()
	endforeach()

	# Consumed by the executable and test targets, which copy it next to their binary — the DLL
	# must sit beside the .exe (or in the working directory) at runtime or the process fails to
	# start with 0xC0000135. CACHE INTERNAL so it also survives into a re-configure; the plain
	# set() above is what makes the FIRST configure correct, which is the whole point of #828.
	set(OLO_STEAM_RUNTIME_DLL "${OLO_STEAM_RUNTIME_DLL}" CACHE INTERNAL "steam_api64.dll to stage next to the executables")
	message(STATUS "Steamworks: using SDK at ${OLO_STEAM_SDK_ROOT}")
endif()
