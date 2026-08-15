#include "OloEnginePCH.h"

// The Dear ImGui Vulkan renderer backend, compiled against the SAME pinned
// 1.4.357 Vulkan headers and volk function pointers as the engine's
// Platform/Vulkan code (#691 Phase 8). Its OWN TU, deliberately: unity-built
// beside the GLFW/OpenGL3 backends the include state broke (VkResult enum
// redefinition against the pinned vulkan_core.h), and here <volk.h> comes
// FIRST — the include-order rule every Platform/Vulkan TU follows (ADR 0011
// amendment 41; the pinned headers must win the include search).
//
// IMGUI_IMPL_VULKAN_USE_VOLK makes imgui_impl_vulkan dispatch through the
// very function pointers volkInitialize/volkLoadInstance loaded — no second
// loader, no vulkan-1.lib prototype linkage (amendment 41a). Keep the macro
// in sync with Platform/Vulkan/VulkanImGuiBackend.cpp, the header's consumer.
#if OLO_WITH_VULKAN
#define IMGUI_IMPL_VULKAN_USE_VOLK
#include <volk.h>

#include <backends/imgui_impl_vulkan.cpp>
#endif
