# NVIDIA Nsight Aftermath — OPTIONAL GPU crash dumps (issue #1198).
#
# What it buys: VK_EXT_device_fault reports a fault ADDRESS and
# VK_NV_device_diagnostic_checkpoints reports the PASS, but neither says which resource lived at
# that address or whether it had already been destroyed. Aftermath's page-fault decoding answers
# exactly that — the VkImage/VkBuffer handle, its format and extent, a Vulkan-only "MemoryFreed"
# residency, and the shader that issued the read. That is the fact a GPU use-after-free
# investigation turns on, and #1198 stalled for want of it.
#
# NEVER VENDORED, for the same reason as Steamworks: the SDK is licensed. It is resolved from
# $ENV{AFTERMATH_SDK_ROOT}, and its absence is the normal case — a build without it compiles
# VulkanAftermath.cpp as a stub that says so rather than going quiet.
#
# WHY THIS LIVES IN THE ROOT AND NOT IN OloEngine/CMakeLists.txt — this is issue #828's lesson,
# paid twice. OLO_AFTERMATH_RUNTIME_DLL is read by olo_copy_aftermath_runtime() from FOUR app
# directories, and OloEngine/CMakeLists.txt runs add_subdirectory(tests) near its top, long before
# its own body could set the variable. Resolving it there left OloEngine-Tests without the DLL,
# and the executable then failed to start during gtest discovery with an error naming nothing
# about Aftermath. Included from the root ABOVE every add_subdirectory(), the variable is always
# set before any consumer can read it.

# An earlier revision cached this; drop any stale entry so a tree reconfigured after the SDK moved
# or went away does not keep staging a DLL that is no longer there.
unset(OLO_AFTERMATH_RUNTIME_DLL CACHE)

set(OLO_AFTERMATH_FOUND OFF)
set(OLO_AFTERMATH_ROOT "")
set(OLO_AFTERMATH_RUNTIME_DLL "")

if(OLO_WITH_VULKAN AND WIN32 AND DEFINED ENV{AFTERMATH_SDK_ROOT} AND NOT "$ENV{AFTERMATH_SDK_ROOT}" STREQUAL "")
	file(TO_CMAKE_PATH "$ENV{AFTERMATH_SDK_ROOT}" _olo_aftermath_root)
	if(EXISTS "${_olo_aftermath_root}/include/GFSDK_Aftermath_GpuCrashDump.h"
	   AND EXISTS "${_olo_aftermath_root}/lib/x64/GFSDK_Aftermath_Lib.x64.lib"
	   AND EXISTS "${_olo_aftermath_root}/lib/x64/GFSDK_Aftermath_Lib.x64.dll")
		set(OLO_AFTERMATH_FOUND ON)
		set(OLO_AFTERMATH_ROOT "${_olo_aftermath_root}")
		set(OLO_AFTERMATH_RUNTIME_DLL "${_olo_aftermath_root}/lib/x64/GFSDK_Aftermath_Lib.x64.dll")
		message(STATUS "Nsight Aftermath SDK found at ${OLO_AFTERMATH_ROOT} - GPU crash dumps available "
		               "(enable at runtime with OLO_VULKAN_AFTERMATH=1)")
	else()
		# Loud, not silent: someone who set the variable meant to get Aftermath.
		message(WARNING
			"AFTERMATH_SDK_ROOT=${_olo_aftermath_root} does not look like an Nsight Aftermath SDK. "
			"Expected include/GFSDK_Aftermath_GpuCrashDump.h plus lib/x64/GFSDK_Aftermath_Lib.x64.{lib,dll}. "
			"Building without Aftermath.")
	endif()
	unset(_olo_aftermath_root)
endif()
