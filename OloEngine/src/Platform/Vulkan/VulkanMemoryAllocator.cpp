#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

// The single VMA_IMPLEMENTATION TU for the engine (#691 Phase 4). The
// function-pointer config must match VulkanContext.cpp exactly: every Vulkan
// entry point reaches VMA through volk's loaded pointers
// (vmaImportVulkanFunctionsFromVolk at allocator creation) — nothing links
// vulkan-1.lib, so static and dynamic self-loading are both disabled.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <volk.h>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#endif // OLO_WITH_VULKAN
