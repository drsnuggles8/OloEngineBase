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

// The ray-tracing capability vocabulary (#978) is API-neutral on purpose, so
// the device can publish its reason and its captured properties in the same
// type the renderer facade returns — one owner for the capability value
// instead of a Vulkan struct here and a parallel enum there.
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // #691: the WINDOW-INDEPENDENT half of the Vulkan bring-up, split
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
        // #691 (the virtual-geometry MDI-count path). All three
        // are enabled-when-supported, never contract rows:
        //  - drawIndirectCount gates vkCmdDrawIndexedIndirectCount (core 1.2,
        //    feature-gated) — MultiDrawElementsIndirectCountRaw drops without it.
        //  - multiDrawIndirect gates maxDrawCount > 1 on the same call
        //    (VUID-vkCmdDrawIndexedIndirectCount-maxDrawCount-02405).
        //  - shaderDrawParameters gates the SPIR-V DrawParameters capability
        //    (gl_DrawID / gl_BaseInstance in VirtualMeshGBuffer.glsl) — module
        //    creation fails validation without it.
        [[nodiscard]] bool IsDrawIndirectCountEnabled() const
        {
            return m_DrawIndirectCountEnabled;
        }
        [[nodiscard]] bool IsMultiDrawIndirectEnabled() const
        {
            return m_MultiDrawIndirectEnabled;
        }
        [[nodiscard]] bool IsShaderDrawParametersEnabled() const
        {
            return m_ShaderDrawParametersEnabled;
        }
        // #691, ADR 0011 §5: VK_EXT_extended_dynamic_state3's three
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
        // VK_EXT_mesh_shader (issue #813): OPTIONAL, enabled when the driver
        // supports BOTH taskShader and meshShader — never an ADR 0010 gate
        // row (requiring it would silently widen the capability contract).
        // TRUE → the facade's SupportsMeshShaders answers yes and
        // DrawMeshTasks records vkCmdDrawMeshTasksEXT; FALSE → mesh-pipeline
        // work is refused loudly upstream (refuse-or-degrade, decided
        // explicitly, never silent).
        [[nodiscard]] bool IsMeshShaderEnabled() const
        {
            return m_MeshShaderEnabled;
        }
        // The physical device's VK_EXT_mesh_shader limits, cached at Init via
        // the backend's one vkGetPhysicalDeviceProperties2 probe. Meaningful
        // only when the extension is present (all-zero otherwise) — gate on
        // IsMeshShaderEnabled before consuming the limits.
        [[nodiscard]] const VkPhysicalDeviceMeshShaderPropertiesEXT& GetMeshShaderProperties() const
        {
            return m_MeshShaderProperties;
        }

        // --- Hardware ray tracing (issue #978) ---------------------------
        // VK_KHR_acceleration_structure + VK_KHR_ray_query (+ the
        // VK_KHR_deferred_host_operations dependency): OPTIONAL, the same
        // Tier-2 rule as VK_EXT_mesh_shader above — never an ADR 0010 gate
        // row, because a device without ray tracing still satisfies the
        // contract and the renderer routes around it explicitly.
        //
        // TRUE means all four things hold, which is what makes this safe to
        // call rather than merely informative: the extensions are listed, the
        // feature bits came back VK_TRUE, vkCreateDevice accepted them, and
        // volk populated the entry points. The reason a FALSE answer is false
        // is carried separately by GetRayTracingUnsupportedReason so the
        // renderer can report it rather than silently disabling itself.
        [[nodiscard]] bool IsRayQueryEnabled() const
        {
            return m_RayQueryEnabled;
        }
        // VK_KHR_ray_tracing_pipeline: a strict superset requirement of the
        // above (an SBT path). Ray query alone is enough for this subsystem,
        // so this stays false on a device that offers acceleration structures
        // without pipelines, and only the SBT alignment properties depend on
        // it.
        [[nodiscard]] bool IsRayTracingPipelineEnabled() const
        {
            return m_RayTracingPipelineEnabled;
        }
        // Why IsRayQueryEnabled() is false. Meaningless (None) when it is
        // true. Stored as the neutral enum rather than a string so the
        // renderer's capability value and the log line cannot drift.
        [[nodiscard]] RayTracing::UnsupportedReason GetRayTracingUnsupportedReason() const
        {
            return m_RayTracingUnsupportedReason;
        }
        // AS alignment / scratch alignment / max geometry-instance-primitive
        // counts, and the SBT alignments when the pipeline extension is on.
        // Captured once at Init through the same vkGetPhysicalDeviceProperties2
        // probe as the mesh-shader limits; all-zero when RT is unavailable, so
        // gate on IsRayQueryEnabled before consuming a number.
        [[nodiscard]] const RayTracing::DeviceProperties& GetRayTracingProperties() const
        {
            return m_RayTracingProperties;
        }

        // --- Vulkan 1.4 core conveniences (issue #809) -------------------
        // These are FEATURES, not extensions, on the ADR 0010 floor: the
        // device already targets 1.4, so no extension name is added and no
        // capability-gate row widens. They are the synchronization2 class
        // though — core-promoted but still DEFAULT OFF at device creation —
        // so each is enabled from a vkGetPhysicalDeviceFeatures2 probe and
        // every use site gates on the flag rather than on the API version.

        // hostImageCopy: TRUE -> the load-time texture upload path writes
        // host memory straight into an optimally-tiled VkImage
        // (vkCopyMemoryToImage + vkTransitionImageLayout), with no staging
        // buffer and no queue submit — which is what removes the
        // one-shot-submit ordering hazard (amendment (72)) for asset loads.
        // FALSE -> every upload keeps the staging + one-shot path.
        [[nodiscard]] bool IsHostImageCopyEnabled() const
        {
            return m_HostImageCopyEnabled;
        }
        // maintenance5: TRUE -> index-buffer binds go through
        // vkCmdBindIndexBuffer2 with the buffer's REAL byte size instead of
        // the implicit whole-buffer bind, so an out-of-range index count is a
        // validation error rather than an out-of-bounds read (amendment (9b)'s
        // DrawIndexed(va, 0) "whole buffer" sentinel resolves to a number the
        // bind can actually state).
        //
        // maintenance6 is deliberately NOT enabled: nothing in the backend
        // uses any of its relaxations, and a feature bit nothing consumes is
        // dead weight the validation layer still has to reason about (the
        // same rule the EDS3 and mesh-shader bits above follow).
        [[nodiscard]] bool IsMaintenance5Enabled() const
        {
            return m_Maintenance5Enabled;
        }

        // Per-format half of the host-image-copy gate: an image may only carry
        // VK_IMAGE_USAGE_HOST_TRANSFER_BIT when its format+tiling advertises
        // VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT
        // (VUID-VkImageCreateInfo-usage-10245), and that is a per-format
        // question no device-level flag answers. Optimal tiling only — the
        // texture classes create nothing else. Memoized; false whenever host
        // image copy is not enabled at all.
        [[nodiscard]] bool SupportsHostImageCopyForFormat(VkFormat format) const;

        // The layouts host image copy may READ FROM / WRITE TO on this driver.
        // Only VK_IMAGE_LAYOUT_GENERAL is guaranteed to be in both lists, so a
        // host path that wants to leave an image in SHADER_READ_ONLY_OPTIMAL
        // (or start one from it) has to ask. Membership is exact: the lists
        // enumerate supported layouts and carry no wildcard entry.
        [[nodiscard]] bool IsHostCopySrcLayoutSupported(VkImageLayout layout) const;
        [[nodiscard]] bool IsHostCopyDstLayoutSupported(VkImageLayout layout) const;
        // vkTransitionImageLayout's newLayout must be in pCopyDstLayouts —
        // VUID-VkHostImageLayoutTransitionInfo-newLayout-09057, checked
        // against the registry's validusage.json rather than assumed. It is
        // NOT "either list": a layout that only appears in pCopySrcLayouts is
        // not a legal transition target, and treating it as one is invalid
        // usage the validation layer would catch only on a driver whose two
        // lists differ.
        [[nodiscard]] bool IsHostTransitionTargetSupported(VkImageLayout layout) const
        {
            return IsHostCopyDstLayoutSupported(layout);
        }

        // VK_EXT_device_fault (enabled when the driver has it): after a
        // VK_ERROR_DEVICE_LOST, logs the driver's fault report — fault type
        // (page fault vs. hang) and the faulting GPU address — so a device
        // loss names its cause instead of just its VkResult. Safe to call
        // any time; no-ops when the extension is absent or no fault is
        // pending. (#691 — added to diagnose the foliage device
        // loss, kept as a permanent post-mortem instrument.)
        void LogDeviceFaultInfo() const;

        // Validation-error counter: the debug messenger increments this on
        // every ERROR-severity validation message. Tests assert it stays 0.
        // Always 0 outside OLO_DEBUG (no messenger exists there).
        static u64 GetValidationErrorCount();
        static void ResetValidationErrorCount();

        // Process-wide accessor for the live device (set by Init, cleared by
        // Shutdown). Null when no Vulkan device is up. Backend resource
        // classes (the VMA-backed transients) reach the allocator
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
        bool m_DrawIndirectCountEnabled = false;
        bool m_MultiDrawIndirectEnabled = false;
        bool m_ShaderDrawParametersEnabled = false;
        bool m_DeviceFaultEnabled = false;
        bool m_MeshShaderEnabled = false;
        bool m_HostImageCopyEnabled = false;
        bool m_Maintenance5Enabled = false;
        // Host-image-copy layout lists, read once after device creation.
        std::vector<VkImageLayout> m_HostCopySrcLayouts;
        std::vector<VkImageLayout> m_HostCopyDstLayouts;
        // Logged, not branched on: the driver's answer to "does
        // VK_IMAGE_USAGE_HOST_TRANSFER_BIT change an image's memory type
        // requirements?". It is VK_FALSE on NVIDIA, which is why the usage
        // bit is kept off render-target-only textures at the texture class
        // (VulkanTexture2D's renderTargetOnly) rather than gated here —
        // gating the feature on this property disables it outright on the
        // hardware it was built for.
        bool m_HostCopyMemoryTypeNeutral = false;
        // SupportsHostImageCopyForFormat's memo. Mutable + locked because the
        // query is logically const and texture creation is not guaranteed to
        // stay on one thread (the asset loader is a thread pool).
        mutable std::mutex m_HostImageCopyFormatMutex;
        mutable std::unordered_map<VkFormat, bool> m_HostImageCopyFormatCache;
        // Cached at Init when VK_EXT_mesh_shader is present; all-zero
        // otherwise (see GetMeshShaderProperties).
        VkPhysicalDeviceMeshShaderPropertiesEXT m_MeshShaderProperties{};

        // Ray tracing (#978). Every one of these is committed AFTER
        // vkCreateDevice returns and AFTER volkLoadDevice has run, because
        // until both have happened the flags would describe a request rather
        // than the logical device (the m_Maintenance5Enabled rule).
        bool m_RayQueryEnabled = false;
        bool m_RayTracingPipelineEnabled = false;
        RayTracing::UnsupportedReason m_RayTracingUnsupportedReason = RayTracing::UnsupportedReason::ExtensionMissing;
        RayTracing::DeviceProperties m_RayTracingProperties{};
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
