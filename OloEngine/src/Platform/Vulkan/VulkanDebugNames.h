#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// =============================================================================
// VulkanDebugNames — VK_EXT_debug_utils object names for engine resources
// (issue #800).
//
// WHY THIS EXISTS: a validation message names the offending object by its
// 64-bit handle only ("expects VkImage 0x291d000000291d ... current layout is
// VK_IMAGE_LAYOUT_UNDEFINED"). Nothing in the engine's log speaks that
// vocabulary, so narrowing a layout error to a resource meant inferring it
// from the surrounding log — which is how #800 stayed open across two phases.
// Naming the object makes the layer print the name inside the message itself.
//
// OPT-IN, via the `OLO_VK_OBJECT_NAMES` environment variable (any value except
// unset, empty, and exactly "0"), and Debug builds only, and only when VK_EXT_debug_utils actually came
// up (the extension is requested alongside VK_LAYER_KHRONOS_validation, so an
// unvalidated run has no entry point). Every entry is a no-op otherwise —
// callers must NOT guard their call sites.
//
// WHY OPT-IN: naming an image at registration puts a layer round-trip on the
// image-CREATION path, which is the path a window-resize storm hammers.
// Naming unconditionally moved the frame enough to hide #800's own race —
// 14 storm rounds with 0 errors, against a reliable 2-per-6 on the same
// build with naming off. A diagnostic that changes the timing of what it is
// meant to identify is worse than none, so you switch it on deliberately.
// =============================================================================

#include "Platform/Vulkan/VulkanDevice.h"

#include <string_view>

namespace OloEngine::VulkanDebugNames
{
    // Attach `name` to `image` (VK_OBJECT_TYPE_IMAGE). No-op outside Debug,
    // without a live device, or when the debug-utils entry point is absent.
    void SetImageName(VkImage image, std::string_view name);

    // The name every image gets at VulkanImageInfoRegistry::Register time:
    // "<label> #<registrationId> <W>x<H> fmt=<n>". The registration id is the
    // one identifier that survives a driver handle recycle, so a message that
    // carries it can be correlated against the layout tracker's own notion of
    // which image it is following.
    [[nodiscard]] bool Enabled();
} // namespace OloEngine::VulkanDebugNames

#endif // OLO_WITH_VULKAN
