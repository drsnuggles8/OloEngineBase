#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

// This header exposes Vulkan types directly — it is included only by
// Platform/Vulkan siblings and by OLO_WITH_VULKAN-guarded engine factory TUs
// (the sanctioned factory-include pattern, rhi-abstraction-boundary.md).
// NEVER include <vulkan/vulkan.h> here or anywhere: volk owns the function
// pointers and the declarations (ADR 0011 amendment 41a — an import-library
// thunk colliding with volk's data symbols is a runtime AV, not a link error).
//
// volk before VMA: vk_mem_alloc.h keys its volk import helper on
// VOLK_HEADER_VERSION.
#include <volk.h>

// Function-pointer config must match VulkanMemoryAllocator.cpp (the
// VMA_IMPLEMENTATION TU) exactly — VMA is imported through volk's loaded
// pointers, never linked statically (nothing links vulkan-1.lib). Guarded so
// a TU that already defined them (with the same value) doesn't warn.
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#endif
#include <vk_mem_alloc.h>

#include <functional>

namespace OloEngine
{
    // #691 Phase 5: the WINDOW-INDEPENDENT half of the Vulkan bring-up, split
    // out of VulkanContext so headless tests (and later the render graph's
    // execution layer) can bring a device up without a window. Owns: volk
    // loader init, VkInstance, debug messenger (OLO_DEBUG only), physical-
    // device selection, VkDevice, the single graphics(+present) queue, the
    // VmaAllocator, and the command pool. Does NOT own: surface, swapchain,
    // per-image present semaphores, per-frame acquire semaphores / fences /
    // command buffers — that presentation and frame-loop state stays in
    // VulkanContext.
    class VulkanDevice
    {
      public:
        VulkanDevice() = default;
        ~VulkanDevice();

        VulkanDevice(const VulkanDevice&) = delete;
        VulkanDevice& operator=(const VulkanDevice&) = delete;
        VulkanDevice(VulkanDevice&&) = delete;
        VulkanDevice& operator=(VulkanDevice&&) = delete;

        // Bring up instance + device. When `surfaceProvider` yields a surface
        // the queue-family pick requires graphics+present on ONE family (the
        // ADR 0010 contract row); when it yields VK_NULL_HANDLE (headless
        // tests) it requires graphics only. The callback is invoked between
        // instance creation and device selection — VulkanContext must create
        // its VkSurfaceKHR AFTER the instance exists but BEFORE the physical-
        // device pick (present support is per queue family, per surface).
        // VulkanDevice uses the returned surface for the pick only; it does
        // NOT own or destroy it — the caller does. Throws std::runtime_error
        // to REFUSE initialisation (never degrade — ADR 0010) when the loader
        // is absent or no device satisfies the capability contract.
        void Init(const std::function<VkSurfaceKHR(VkInstance)>& surfaceProvider);
        void Shutdown(); // also called by dtor; idempotent

        [[nodiscard]] VkInstance GetInstance() const
        {
            return m_Instance;
        }
        [[nodiscard]] VkPhysicalDevice GetPhysicalDevice() const
        {
            return m_PhysicalDevice;
        }
        [[nodiscard]] VkDevice GetDevice() const
        {
            return m_Device;
        }
        [[nodiscard]] u32 GetQueueFamily() const
        {
            return m_QueueFamily;
        }
        [[nodiscard]] VkQueue GetQueue() const
        {
            return m_Queue;
        }
        [[nodiscard]] VmaAllocator GetAllocator() const
        {
            return m_Allocator;
        }
        [[nodiscard]] VkCommandPool GetCommandPool() const
        {
            return m_CommandPool;
        }

        // Core features enabled at device creation (when supported — never
        // required, they are not contract rows). Barrier stage masks may only
        // name stages whose features are ENABLED (VUID-…-03929/-03930), so
        // VulkanRendererAPI narrows its shader-stage unions by these.
        [[nodiscard]] bool IsTessellationShaderEnabled() const
        {
            return m_TessellationShaderEnabled;
        }
        [[nodiscard]] bool IsGeometryShaderEnabled() const
        {
            return m_GeometryShaderEnabled;
        }
        // ENABLED on the logical device (not merely supported by the physical
        // one) — the facade's SupportsInt64ShaderAtomics must report what a
        // shader can actually use.
        [[nodiscard]] bool IsShaderBufferInt64AtomicsEnabled() const
        {
            return m_ShaderBufferInt64AtomicsEnabled;
        }
        // Phase 6 (#691, ADR 0011 §5): VK_EXT_extended_dynamic_state3's three
        // blend states (enable/equation/write-mask) are enabled when the driver
        // has them. TRUE → the pipeline builder makes blend state dynamic;
        // FALSE → blend is baked into the PSO key (the fallback path the ADR
        // predicts never triggers on the NVIDIA/AMD desktop floor, but the
        // builder must still take the other branch — same rule as
        // IsBindlessSupported()).
        [[nodiscard]] bool IsDynamicBlendStateEnabled() const
        {
            return m_DynamicBlendStateEnabled;
        }

        // Validation-error counter: the debug messenger increments this on
        // every ERROR-severity validation message. Tests assert it stays 0.
        // Always 0 outside OLO_DEBUG (no messenger exists there).
        static u64 GetValidationErrorCount();
        static void ResetValidationErrorCount();

        // Process-wide accessor for the live device (set by Init, cleared by
        // Shutdown). Null when no Vulkan device is up. Backend resource
        // classes (Phase 5's VMA-backed transients) reach the allocator
        // through this.
        static VulkanDevice* Get();

      private:
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_Device = VK_NULL_HANDLE;
        u32 m_QueueFamily = 0;
        VkQueue m_Queue = VK_NULL_HANDLE;
        VmaAllocator m_Allocator = VK_NULL_HANDLE;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        bool m_TessellationShaderEnabled = false;
        bool m_GeometryShaderEnabled = false;
        bool m_ShaderBufferInt64AtomicsEnabled = false;
        bool m_DynamicBlendStateEnabled = false;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
