#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanCapabilities.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The vendored Vulkan-Headers (SDK 1.4.357.0, the ADR 0010 tooling floor) must be
// the ones this TU compiles against. An installed SDK's include dir is also on the
// include path (via the shaderc toolchain's find_package(Vulkan)); if it ever wins
// the include search, an older SDK would break the VK_EXT_descriptor_heap
// declarations silently — fail the build instead.
static_assert(VK_HEADER_VERSION >= 357,
              "Vulkan headers older than the vendored 1.4.357 floor — the installed SDK's include dir "
              "won the include search over OloEngine/vendor's Vulkan-Headers (see vendor/CMakeLists.txt)");

namespace OloEngine
{
    namespace
    {
        // Kept in sync with VulkanContext.cpp's copy (both anonymous-namespace,
        // trivially small — not worth a shared header).
        void VkCheck(VkResult result, const char* what)
        {
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(std::string("Vulkan bring-up: ") + what + " failed (VkResult " +
                                         std::to_string(static_cast<int>(result)) + ")");
            }
        }

        // ERROR-severity validation messages seen by the debug messenger. Always
        // defined (the public accessors exist in every config); only the
        // OLO_DEBUG messenger ever increments it.
        std::atomic<u64> s_ValidationErrorCount{ 0 };

        // The live device, set at the end of a successful Init and cleared by
        // Shutdown. A plain pointer: single-threaded bring-up/teardown, and the
        // Init assert refuses a second live device.
        VulkanDevice* s_ActiveDevice = nullptr;

#ifdef OLO_DEBUG
        VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT /*types*/,
            const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
            void* /*userData*/)
        {
            const char* message = (callbackData != nullptr && callbackData->pMessage != nullptr)
                                      ? callbackData->pMessage
                                      : "(no message)";
            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
            {
                s_ValidationErrorCount.fetch_add(1, std::memory_order_relaxed);
                OLO_CORE_ERROR("[Vulkan] {}", message);
            }
            else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
            {
                OLO_CORE_WARN("[Vulkan] {}", message);
            }
            else
            {
                OLO_CORE_TRACE("[Vulkan] {}", message);
            }
            return VK_FALSE;
        }

        VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerCreateInfo()
        {
            VkDebugUtilsMessengerCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = DebugMessengerCallback;
            return info;
        }

        bool ValidationLayerAvailable()
        {
            u32 count = 0;
            if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
            {
                return false;
            }
            std::vector<VkLayerProperties> layers(count);
            if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
            {
                return false;
            }
            for (const VkLayerProperties& layer : layers)
            {
                if (std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation")
                {
                    return true;
                }
            }
            return false;
        }
#endif
    } // namespace

    VulkanDevice::~VulkanDevice()
    {
        Shutdown();
    }

    void VulkanDevice::Init(const std::function<VkSurfaceKHR(VkInstance)>& surfaceProvider)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(s_ActiveDevice == nullptr, "VulkanDevice::Init: another VulkanDevice is already live");
        OLO_CORE_ASSERT(m_Instance == VK_NULL_HANDLE, "VulkanDevice::Init called twice");

        // --- Loader ---------------------------------------------------------
        // Safe to run twice: VulkanContext also runs it (before this call) so
        // its GLFW support probe can fail fast before any instance work.
        if (volkInitialize() != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Vulkan bring-up: no Vulkan loader found on this system. Run with --rhi=opengl.");
        }

        // --- Instance -------------------------------------------------------
        // The window-independent half cannot ask GLFW which surface extensions
        // the platform needs (glfwGetRequiredInstanceExtensions is a window
        // concern), so it enables VK_KHR_surface plus whichever platform
        // surface extension the instance offers. On Windows that is exactly
        // GLFW's list (VK_KHR_surface + VK_KHR_win32_surface). Headless runs
        // need VK_KHR_surface enabled too: the ADR 0010 contract's
        // VK_KHR_swapchain DEVICE extension declares an instance-level
        // dependency on it.
        u32 availableExtensionCount = 0;
        VkCheck(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr),
                "vkEnumerateInstanceExtensionProperties");
        std::vector<VkExtensionProperties> availableExtensions(availableExtensionCount);
        VkCheck(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, availableExtensions.data()),
                "vkEnumerateInstanceExtensionProperties");
        auto extensionAvailable = [&availableExtensions](std::string_view name)
        {
            for (const VkExtensionProperties& extension : availableExtensions)
            {
                if (std::string_view(extension.extensionName) == name)
                {
                    return true;
                }
            }
            return false;
        };

        // Names as literals: the platform-specific VK_KHR_*_EXTENSION_NAME
        // macros live in headers volk only pulls under VK_USE_PLATFORM_* defines.
        constexpr const char* kSurfaceExtensionNames[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            "VK_KHR_win32_surface",
            "VK_KHR_xcb_surface",
            "VK_KHR_xlib_surface",
            "VK_KHR_wayland_surface",
        };
        std::vector<const char*> instanceExtensions;
        for (const char* name : kSurfaceExtensionNames)
        {
            if (extensionAvailable(name))
            {
                instanceExtensions.push_back(name);
            }
        }
        std::vector<const char*> instanceLayers;

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OloEngine";
        appInfo.pEngineName = "OloEngine";
        appInfo.apiVersion = VulkanCapabilities::kMinApiVersion;

        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;

#ifdef OLO_DEBUG
        // Chained into pNext so create/destroy of the instance itself is covered
        // before/after the persistent messenger exists.
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeDebugMessengerCreateInfo();
        // Synchronization validation is the real test of Phase 5's barrier
        // translation: the core validation layer checks structure, sync
        // validation checks that the barriers actually cover every hazard.
        const VkValidationFeatureEnableEXT enabledValidationFeatures[] = {
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
        };
        VkValidationFeaturesEXT validationFeatures{};
        validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validationFeatures.enabledValidationFeatureCount =
            static_cast<u32>(std::size(enabledValidationFeatures));
        validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures;
        const bool useValidation = ValidationLayerAvailable();
        if (useValidation)
        {
            instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            // Provided by the validation layer itself, so only requestable when
            // the layer is enabled.
            instanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            // Chain: instance -> validation features -> messenger (both are
            // consumed by the layer at instance create/destroy).
            validationFeatures.pNext = &messengerInfo;
            instanceInfo.pNext = &validationFeatures;
        }
        else
        {
            OLO_CORE_WARN("[Vulkan] VK_LAYER_KHRONOS_validation not available — running unvalidated");
        }
#endif
        instanceInfo.enabledExtensionCount = static_cast<u32>(instanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
        instanceInfo.enabledLayerCount = static_cast<u32>(instanceLayers.size());
        instanceInfo.ppEnabledLayerNames = instanceLayers.empty() ? nullptr : instanceLayers.data();

        VkCheck(vkCreateInstance(&instanceInfo, nullptr, &m_Instance), "vkCreateInstance");
        volkLoadInstance(m_Instance);

#ifdef OLO_DEBUG
        if (useValidation)
        {
            VkCheck(vkCreateDebugUtilsMessengerEXT(m_Instance, &messengerInfo, nullptr, &m_DebugMessenger),
                    "vkCreateDebugUtilsMessengerEXT");
        }
#endif

        // --- Surface (owned by the CALLER, borrowed here for the pick) -------
        const VkSurfaceKHR surface = surfaceProvider ? surfaceProvider(m_Instance) : VK_NULL_HANDLE;

        // --- Physical device: gate HARD on ADR 0010's capability contract ----
        u32 deviceCount = 0;
        VkCheck(vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr), "vkEnumeratePhysicalDevices");
        if (deviceCount == 0)
        {
            throw std::runtime_error("Vulkan bring-up: no Vulkan devices present. Run with --rhi=opengl.");
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        VkCheck(vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

        // With a surface: find a queue family doing BOTH graphics and present.
        // Split-family hardware is refused for bring-up — every desktop GPU this
        // backend's hardware floor admits has a combined family, and supporting
        // the split doubles the sync/sharing surface for no gain. Headless
        // (surface == VK_NULL_HANDLE): graphics alone suffices — there is
        // nothing to present to.
        auto findQueueFamily = [surface](VkPhysicalDevice device) -> std::optional<u32>
        {
            u32 familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
            for (u32 i = 0; i < familyCount; ++i)
            {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
                {
                    continue;
                }
                if (surface == VK_NULL_HANDLE)
                {
                    return i;
                }
                VkBool32 presentSupport = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
                if (presentSupport == VK_TRUE)
                {
                    return i;
                }
            }
            return std::nullopt;
        };

        VulkanCapabilityReport bestRejected;
        for (VkPhysicalDevice candidate : devices)
        {
            VulkanCapabilityReport report = VulkanCapabilities::Evaluate(candidate);
            const std::optional<u32> family = findQueueFamily(candidate);
            if (!family.has_value())
            {
                report.Missing.emplace_back(surface != VK_NULL_HANDLE
                                                ? "a combined graphics+present queue family"
                                                : "a graphics queue family");
                report.Satisfied = false;
            }
            if (report.Satisfied)
            {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(candidate, &properties);
                // First satisfying discrete GPU wins; a satisfying integrated one is
                // kept only until a discrete appears.
                if (m_PhysicalDevice == VK_NULL_HANDLE ||
                    properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                {
                    m_PhysicalDevice = candidate;
                    m_QueueFamily = *family;
                    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                    {
                        break;
                    }
                }
            }
            else if (bestRejected.Missing.empty() || report.Missing.size() < bestRejected.Missing.size())
            {
                bestRejected = report;
            }
        }

        if (m_PhysicalDevice == VK_NULL_HANDLE)
        {
            // Refuse to initialise, naming the missing capability — never degrade
            // (ADR 0010; the OpenGL fallback is a USER decision, not an engine one).
            std::string missing;
            for (const std::string& item : bestRejected.Missing)
            {
                missing += (missing.empty() ? "" : ", ") + item;
            }
            throw std::runtime_error(
                "Vulkan bring-up: no device satisfies the capability contract (ADR 0010). Closest device '" +
                bestRejected.DeviceName + "' is missing: " + missing + ". Run with --rhi=opengl.");
        }

        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);
            OLO_CORE_INFO("[Vulkan] Device: {} (driver {}.{}.{}, API {}.{}.{})", properties.deviceName,
                          VK_API_VERSION_MAJOR(properties.driverVersion), VK_API_VERSION_MINOR(properties.driverVersion),
                          VK_API_VERSION_PATCH(properties.driverVersion), VK_API_VERSION_MAJOR(properties.apiVersion),
                          VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
        }

        // --- Logical device + queue -----------------------------------------
        const f32 queuePriority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = m_QueueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        // Enable the contract's feature bits at the gate: a driver that
        // advertises the features but rejects enabling them should fail HERE,
        // not later at first descriptor-heap use.
        VkPhysicalDeviceDescriptorHeapFeaturesEXT heapFeatures{};
        heapFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        heapFeatures.descriptorHeap = VK_TRUE;
        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedFeatures{};
        untypedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
        untypedFeatures.shaderUntypedPointers = VK_TRUE;
        untypedFeatures.pNext = &heapFeatures;

        // synchronization2 backs the vkCmdPipelineBarrier2/vkQueueSubmit2 calls in
        // SwapBuffers. Core in 1.3 and MANDATORY for 1.3+ devices, so it needs no
        // capability-gate entry — but core-promoted features still default OFF at
        // device creation, and validation flags every sync2 call without this bit
        // (the NVIDIA driver happens to tolerate it, which is exactly the kind of
        // works-by-accident the validation layers exist to catch).
        VkPhysicalDeviceVulkan13Features vulkan13Features{};
        vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13Features.synchronization2 = VK_TRUE;
        // glslang at vulkan1.4 lowers `discard` to OpDemoteToHelperInvocation
        // — without the feature every discard shader fails module creation
        // (VUID 08740; found by the fluid splat shaders, issue #691 Phase 7).
        vulkan13Features.shaderDemoteToHelperInvocation = VK_TRUE;
        // dynamicRendering backs Phase 6's VkPipelineRenderingCreateInfo pipelines
        // (no VkRenderPass objects anywhere in the backend). Same class as
        // synchronization2: core in 1.3 and MANDATORY for 1.3+ devices, so no
        // capability-gate entry — but it still defaults OFF at device creation.
        vulkan13Features.dynamicRendering = VK_TRUE;
        vulkan13Features.pNext = &untypedFeatures;

        // Phase 6 (#691): two more core-promoted-but-default-OFF features, both
        // MANDATORY at the 1.3+ floor so neither is a capability-gate row:
        //  - timelineSemaphore backs RHI::GpuFence (ADR 0011 §6 — the split-barrier
        //    signal/wait primitive IS a timeline semaphore).
        //  - bufferDeviceAddress backs the root-data pointer model (ADR 0011 §4):
        //    the frame arena hands out VkDeviceAddress values the shader-side
        //    binding mappings dereference, and VK_EXT_descriptor_heap itself
        //    depends on it (the heap is addressed as a plain BDA range).
        // shaderBufferInt64Atomics lives HERE, not in a standalone
        // VkPhysicalDeviceShaderAtomicInt64Features: the feature was promoted
        // in 1.2 and VUID-VkDeviceCreateInfo-pNext-02830 forbids chaining both
        // structs (caught by validation on Phase 6's first device run — the
        // standalone struct was fine only while no Vulkan12Features existed).
        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.timelineSemaphore = VK_TRUE;
        vulkan12Features.bufferDeviceAddress = VK_TRUE;
        vulkan12Features.pNext = &vulkan13Features;

        // #691 Phase 7 Wave C: shaderDrawParameters backs the SPIR-V
        // DrawParameters capability (gl_DrawID / gl_BaseInstance — the
        // virtual-geometry MDI shaders). Promoted to Vulkan11Features and
        // enabled WHEN SUPPORTED (never a gate row); drawIndirectCount (set
        // below from the same probe) gates vkCmdDrawIndexedIndirectCount.
        VkPhysicalDeviceVulkan11Features vulkan11Features{};
        vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan11Features.pNext = &vulkan12Features;

        std::vector<const char*> deviceExtensions = VulkanCapabilities::RequiredDeviceExtensions();

        // VK_EXT_extended_dynamic_state3: OPTIONAL (ADR 0011 §5 — dynamic blend
        // state when available, blend baked into the PSO when not). Never a gate
        // row; requiring it would silently widen the ADR 0010 contract. Only the
        // three blend states Phase 6 uses are enabled — enabling feature bits a
        // pipeline never sets dynamic would be dead weight the validation layer
        // still has to reason about.
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT supportedEds3{};
        supportedEds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
        bool hasEds3Extension = false;
        {
            u32 extCount = 0;
            vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> available(extCount);
            vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, available.data());
            hasEds3Extension = std::ranges::any_of(available, [](const VkExtensionProperties& p)
                                                   { return std::string_view(p.extensionName) == VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME; });
            if (hasEds3Extension)
            {
                VkPhysicalDeviceFeatures2 probe{};
                probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                probe.pNext = &supportedEds3;
                vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &probe);
            }
        }
        const bool wantDynamicBlend = hasEds3Extension &&
                                      supportedEds3.extendedDynamicState3ColorBlendEnable == VK_TRUE &&
                                      supportedEds3.extendedDynamicState3ColorBlendEquation == VK_TRUE &&
                                      supportedEds3.extendedDynamicState3ColorWriteMask == VK_TRUE;
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3Features{};
        eds3Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
        if (wantDynamicBlend)
        {
            deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
            eds3Features.extendedDynamicState3ColorBlendEnable = VK_TRUE;
            eds3Features.extendedDynamicState3ColorBlendEquation = VK_TRUE;
            eds3Features.extendedDynamicState3ColorWriteMask = VK_TRUE;
            eds3Features.pNext = &vulkan11Features;
        }
        // m_DynamicBlendStateEnabled is committed AFTER vkCreateDevice
        // succeeds (below): the flag must describe the LOGICAL device's
        // enabled features, and until the create returns, nothing has been
        // enabled anywhere.

        // Core VkPhysicalDeviceFeatures the engine's shader stages need:
        // tessellation (terrain patches) and geometry shaders. Enabled WHEN
        // SUPPORTED, never required — these are not ADR 0010 contract rows,
        // and requiring them would silently widen the gate. Barrier stage
        // masks may only name stages whose features are ENABLED
        // (VUID-VkImageMemoryBarrier2-dstStageMask-03929/-03930), so
        // VulkanRendererAPI narrows its shader-stage unions by the two flags
        // recorded here.
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supported);
        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.tessellationShader = supported.tessellationShader;
        enabledFeatures.geometryShader = supported.geometryShader;
        // multiDrawIndirect: maxDrawCount > 1 on vkCmdDrawIndexedIndirectCount
        // requires the feature (#691 Phase 7 Wave C, same when-supported rule).
        enabledFeatures.multiDrawIndirect = supported.multiDrawIndirect;
        // Vertex-stage SSBO writes (ShaderDebugDraw's channel buffers) and
        // fragment-stage storage images (VirtualGeometry's debug images) are
        // rejected at pipeline creation without these two core features
        // (#691 Phase 7 Wave C batch 2, when-supported rule as above).
        enabledFeatures.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
        enabledFeatures.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
        // samplerCubeArray in a shader declares the SampledCubeArray SPIR-V
        // capability, which needs this feature or vkCreateShaderModule refuses
        // the module (VUID-…-08740). The distance-impostor reflection probes
        // (#705) are the first cube-ARRAY sampler in the engine — amendment
        // (65)'s "the feature list grows with each shader family" arriving from
        // master rather than from a new Vulkan pass.
        enabledFeatures.imageCubeArray = supported.imageCubeArray;
        // independentBlend: WITHOUT it every element of
        // VkPipelineColorBlendStateCreateInfo::pAttachments must be IDENTICAL
        // (VUID-VkPipelineColorBlendStateCreateInfo-pAttachments-00605), and
        // the same rule applies to the EDS3 dynamic-blend setters. The facade's
        // whole per-attachment family — SetBlendStateForAttachment /
        // SetBlendFuncForAttachment / SetColorMaskForAttachment, i.e. GL's
        // glEnablei/glBlendFunci/glColorMaski — exists to make those elements
        // DIFFER (WB-OIT's accum-vs-revealage split, and the decal G-Buffer
        // mode matrix's per-RT colour masks). Enabled when supported, never a
        // gate row (#691 Phase 7 Wave C batch 3).
        enabledFeatures.independentBlend = supported.independentBlend;
        m_TessellationShaderEnabled = supported.tessellationShader == VK_TRUE;
        m_GeometryShaderEnabled = supported.geometryShader == VK_TRUE;
        m_MultiDrawIndirectEnabled = supported.multiDrawIndirect == VK_TRUE;

        // shaderBufferInt64Atomics: the facade REPORTS this capability
        // (SupportsInt64ShaderAtomics feeds the virtual-geometry software
        // rasterizer's path choice), and a queried-but-not-enabled feature is
        // a Phase 6 shader crash waiting to happen. Enabled when supported,
        // never required — set on vulkan12Features (see the comment there).
        VkPhysicalDeviceShaderAtomicInt64Features supportedAtomics{};
        supportedAtomics.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
        // Probe the promoted 1.1/1.2 feature structs in the same query:
        // shaderDrawParameters (1.1) + drawIndirectCount (1.2), see above.
        VkPhysicalDeviceVulkan11Features supported11{};
        supported11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        supported11.pNext = &supportedAtomics;
        VkPhysicalDeviceVulkan12Features supported12{};
        supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        supported12.pNext = &supported11;
        VkPhysicalDeviceFeatures2 supportedFeatures2{};
        supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supportedFeatures2.pNext = &supported12;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &supportedFeatures2);
        vulkan12Features.shaderBufferInt64Atomics = supportedAtomics.shaderBufferInt64Atomics;
        // shaderInt64 is a DIFFERENT feature from shaderBufferInt64Atomics:
        // the first is "the shader may declare the Int64 capability at all",
        // the second is "64-bit atomics on buffers". A SPIR-V module that
        // uses 64-bit integers declares Int64 and is rejected by
        // vkCreateShaderModule (VUID-VkShaderModuleCreateInfo-pCode-08740)
        // when only the atomics feature is on — which is exactly what
        // VirtualClusterRaster_Int64 hit: SupportsInt64ShaderAtomics said
        // yes, VirtualGeometryPass::Init built the Int64 variant, the module
        // creation failed with a validation error, and the pass silently fell
        // back to the portable rasteriser. The capability the facade reports
        // needs BOTH halves, so both are enabled and both gate the flag.
        enabledFeatures.shaderInt64 = supported.shaderInt64;
        m_ShaderBufferInt64AtomicsEnabled =
            supportedAtomics.shaderBufferInt64Atomics == VK_TRUE && supported.shaderInt64 == VK_TRUE;
        vulkan12Features.drawIndirectCount = supported12.drawIndirectCount;
        m_DrawIndirectCountEnabled = supported12.drawIndirectCount == VK_TRUE;
        vulkan11Features.shaderDrawParameters = supported11.shaderDrawParameters;
        m_ShaderDrawParametersEnabled = supported11.shaderDrawParameters == VK_TRUE;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = wantDynamicBlend ? static_cast<void*>(&eds3Features)
                                            : static_cast<void*>(&vulkan11Features);
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        deviceInfo.pEnabledFeatures = &enabledFeatures;

        VkCheck(vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device), "vkCreateDevice");
        // The create succeeded with the EDS3 chain (when requested), so the
        // three blend feature bits are now enabled facts of the logical
        // device — commit the flag the pipeline builder branches on.
        m_DynamicBlendStateEnabled = wantDynamicBlend;
        volkLoadDevice(m_Device);
        vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);

        // --- VMA (vendoring proof + the allocator Phase 5's VMA-backed --------
        // transient resources allocate from, reached via VulkanDevice::Get())
        {
            VmaVulkanFunctions vulkanFunctions{};
            VmaAllocatorCreateInfo allocatorInfo{};
            allocatorInfo.physicalDevice = m_PhysicalDevice;
            allocatorInfo.device = m_Device;
            allocatorInfo.instance = m_Instance;
            allocatorInfo.vulkanApiVersion = VulkanCapabilities::kMinApiVersion;
            // bufferDeviceAddress is enabled above (Phase 6); without this flag
            // VMA refuses to pass VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            // through to its pooled allocations and vkGetBufferDeviceAddress
            // on a VMA buffer is undefined.
            allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
            VkCheck(vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &vulkanFunctions),
                    "vmaImportVulkanFunctionsFromVolk");
            allocatorInfo.pVulkanFunctions = &vulkanFunctions;
            VkCheck(vmaCreateAllocator(&allocatorInfo, &m_Allocator), "vmaCreateAllocator");
        }

        // --- Command pool -----------------------------------------------------
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_QueueFamily;
        VkCheck(vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CommandPool), "vkCreateCommandPool");

        s_ActiveDevice = this;
    }

    void VulkanDevice::Shutdown()
    {
        // Idempotent: also runs from the dtor after an explicit Shutdown, and
        // after a partially-failed Init (whatever came up gets torn down).
        // Order: pool -> VMA -> device -> messenger -> instance — the tail of
        // Phase 4's teardown, unchanged. The caller destroys its surface and
        // frame-loop objects BEFORE calling this (surface before instance).
        if (m_Device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_Device);
            if (m_CommandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
                m_CommandPool = VK_NULL_HANDLE;
            }
            if (m_Allocator != VK_NULL_HANDLE)
            {
                vmaDestroyAllocator(m_Allocator);
                m_Allocator = VK_NULL_HANDLE;
            }
            vkDestroyDevice(m_Device, nullptr);
            m_Device = VK_NULL_HANDLE;
            m_Queue = VK_NULL_HANDLE;
        }
        if (m_Instance != VK_NULL_HANDLE)
        {
#ifdef OLO_DEBUG
            if (m_DebugMessenger != VK_NULL_HANDLE)
            {
                vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
                m_DebugMessenger = VK_NULL_HANDLE;
            }
#endif
            vkDestroyInstance(m_Instance, nullptr);
            m_Instance = VK_NULL_HANDLE;
        }
        m_PhysicalDevice = VK_NULL_HANDLE;
        m_QueueFamily = 0;
        if (s_ActiveDevice == this)
        {
            s_ActiveDevice = nullptr;
        }
    }

    u64 VulkanDevice::GetValidationErrorCount()
    {
        return s_ValidationErrorCount.load(std::memory_order_relaxed);
    }

    void VulkanDevice::ResetValidationErrorCount()
    {
        s_ValidationErrorCount.store(0, std::memory_order_relaxed);
    }

    VulkanDevice* VulkanDevice::Get()
    {
        return s_ActiveDevice;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
