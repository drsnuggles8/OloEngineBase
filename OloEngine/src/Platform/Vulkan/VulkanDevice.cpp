#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanCapabilities.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanSecondaryCommandPools.h"

#include "OloEngine/Core/DebugLevers.h"

#include <algorithm>
#include <atomic>
#include <mutex>
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
                // The command-buffer label stack the offending command sat
                // inside. `pMessage` never carries it, so without this an
                // error recorded deep in the frame can only be narrowed by
                // inference — which is exactly the cost issue #800 paid.
                // Costs nothing: the layer already assembled the list.
                if (callbackData != nullptr && callbackData->cmdBufLabelCount > 0u &&
                    callbackData->pCmdBufLabels != nullptr)
                {
                    std::string stack;
                    for (u32 i = 0; i < callbackData->cmdBufLabelCount; ++i)
                    {
                        const char* name = callbackData->pCmdBufLabels[i].pLabelName;
                        if (name == nullptr)
                            continue;
                        if (!stack.empty())
                            stack += " / ";
                        stack += name;
                    }
                    if (!stack.empty())
                        OLO_CORE_ERROR("[Vulkan]   ...inside command-buffer label region: {}", stack);
                }
            }
            else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
            {
                // Two SELF-DECLARED-benign interface classes drop to trace —
                // both message texts literally state the write is legal and
                // merely discarded, and both fire for every pipeline whose
                // consuming stage skips a shared-include varying
                // (InstanceBlock's flat v_InstanceIndex under depth-only /
                // tess consumers, MRT shaders on narrower attachment sets).
                // A real interface ERROR (consumer reads an undeclared input)
                // is invalid usage and arrives at ERROR severity — this
                // filter cannot swallow it.
                const std::string_view text(message);
                const bool benignInterfaceNoise =
                    text.find("but there is no corresponding Input declared") != std::string_view::npos ||
                    text.find("this write is unused") != std::string_view::npos;
                if (benignInterfaceNoise)
                {
                    // Validation cannot name the pipeline's shader; the
                    // callback runs synchronously during recording, so the
                    // currently-bound shader IS the offender — name it.
                    const auto* bound = VulkanShader::GetCurrentlyBound();
                    OLO_CORE_TRACE("[Vulkan] (bound shader: '{}') {}",
                                   bound != nullptr ? bound->GetName() : "<none>", message);
                }
                else
                {
                    OLO_CORE_WARN("[Vulkan] {}", message);
                }
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
        // Synchronization validation is the real test of the barrier
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
        // (VUID 08740; found by the fluid splat shaders, issue #691).
        vulkan13Features.shaderDemoteToHelperInvocation = VK_TRUE;
        // dynamicRendering backs the VkPipelineRenderingCreateInfo pipelines
        // (no VkRenderPass objects anywhere in the backend). Same class as
        // synchronization2: core in 1.3 and MANDATORY for 1.3+ devices, so no
        // capability-gate entry — but it still defaults OFF at device creation.
        vulkan13Features.dynamicRendering = VK_TRUE;
        // maintenance4 backs OpExecutionMode LocalSizeId, which glslang emits
        // for any workgroup-sized stage at SPIR-V 1.6 (the vulkan_1_4 tier's
        // level) — surfaced by the first task/mesh modules (issue #813,
        // VUID-RuntimeSpirv-LocalSizeId-06434). Same class again: core in 1.3
        // and MANDATORY there, default OFF at device creation.
        vulkan13Features.maintenance4 = VK_TRUE;
        vulkan13Features.pNext = &untypedFeatures;

        // #691: two more core-promoted-but-default-OFF features, both
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
        // structs (caught by validation on the first device run — the
        // standalone struct was fine only while no Vulkan12Features existed).
        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.timelineSemaphore = VK_TRUE;
        vulkan12Features.bufferDeviceAddress = VK_TRUE;
        vulkan12Features.pNext = &vulkan13Features;

        // #691: shaderDrawParameters backs the SPIR-V
        // DrawParameters capability (gl_DrawID / gl_BaseInstance — the
        // virtual-geometry MDI shaders). Promoted to Vulkan11Features and
        // enabled WHEN SUPPORTED (never a gate row); drawIndirectCount (set
        // below from the same probe) gates vkCmdDrawIndexedIndirectCount.
        VkPhysicalDeviceVulkan11Features vulkan11Features{};
        vulkan11Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan11Features.pNext = &vulkan12Features;

        // #809: the 1.4 aggregate. Its two consumed bits (hostImageCopy,
        // maintenance5) are filled in from the support probe below. Both are
        // the synchronization2 class: core at the ADR 0010 floor, MANDATORY
        // for a 1.4 device, and still default OFF at device creation.
        //
        // Amendment (51) sweep, done at the moment this struct joined the
        // chain: nothing else chained here is a feature that 1.4 promoted, so
        // there is nothing to fold in. VK_EXT_descriptor_heap,
        // VK_KHR_shader_untyped_pointers, VK_EXT_extended_dynamic_state3,
        // VK_EXT_device_fault and VK_EXT_mesh_shader are all post-1.4 or
        // never-promoted, so each stays a standalone struct. A future
        // promoted-into-1.4 feature MUST move into this struct instead
        // (VUID-VkDeviceCreateInfo-pNext-02830 rejects both spellings at once).
        VkPhysicalDeviceVulkan14Features vulkan14Features{};
        vulkan14Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        vulkan14Features.pNext = &vulkan11Features;

        std::vector<const char*> deviceExtensions = VulkanCapabilities::RequiredDeviceExtensions();

        // VK_EXT_extended_dynamic_state3: OPTIONAL (ADR 0011 §5 — dynamic blend
        // state when available, blend baked into the PSO when not). Never a gate
        // row; requiring it would silently widen the ADR 0010 contract. Only the
        // three blend states the pipelines use are enabled — enabling feature bits a
        // pipeline never sets dynamic would be dead weight the validation layer
        // still has to reason about.
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT supportedEds3{};
        supportedEds3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
        bool hasEds3Extension = false;
        // VK_EXT_device_fault: OPTIONAL post-mortem instrument (see
        // LogDeviceFaultInfo). Same when-supported rule as EDS3 — never a
        // gate row.
        VkPhysicalDeviceFaultFeaturesEXT supportedFault{};
        supportedFault.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        bool hasDeviceFaultExtension = false;
        // VK_EXT_mesh_shader (issue #813): OPTIONAL, same when-supported rule
        // as EDS3 / device-fault — never an ADR 0010 gate row (requiring it
        // would silently widen the contract; the facade's SupportsMeshShaders
        // exists precisely so callers can refuse-or-degrade explicitly).
        VkPhysicalDeviceMeshShaderFeaturesEXT supportedMeshShader{};
        supportedMeshShader.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        bool hasMeshShaderExtension = false;
        // Hardware ray tracing (issue #978): OPTIONAL, same when-supported
        // rule again. Three extensions move together —
        // VK_KHR_acceleration_structure needs
        // VK_KHR_deferred_host_operations as a hard dependency (it has no
        // feature struct of its own, so it is a name in the list only), and
        // VK_KHR_ray_query is what lets a compute/fragment shader trace
        // without an SBT. VK_KHR_ray_tracing_pipeline is probed separately
        // because this subsystem does not need it; only the SBT alignment
        // properties do.
        VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAccelStruct{};
        supportedAccelStruct.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR supportedRayQuery{};
        supportedRayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR supportedRayPipeline{};
        supportedRayPipeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        bool hasAccelStructExtension = false;
        bool hasDeferredHostOpsExtension = false;
        bool hasRayQueryExtension = false;
        bool hasRayPipelineExtension = false;
        {
            u32 extCount = 0;
            vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> available(extCount);
            vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, available.data());
            const auto listed = [&available](const char* name)
            {
                return std::ranges::any_of(available, [name](const VkExtensionProperties& p)
                                           { return std::string_view(p.extensionName) == std::string_view(name); });
            };
            hasEds3Extension = listed(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
            hasDeviceFaultExtension = listed(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            hasMeshShaderExtension = listed(VK_EXT_MESH_SHADER_EXTENSION_NAME);
            hasAccelStructExtension = listed(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            hasDeferredHostOpsExtension = listed(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            hasRayQueryExtension = listed(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            hasRayPipelineExtension = listed(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            if (hasEds3Extension || hasDeviceFaultExtension || hasMeshShaderExtension || hasAccelStructExtension ||
                hasRayQueryExtension || hasRayPipelineExtension)
            {
                VkPhysicalDeviceFeatures2 probe{};
                probe.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                probe.pNext = hasEds3Extension ? static_cast<void*>(&supportedEds3) : nullptr;
                if (hasDeviceFaultExtension)
                {
                    supportedFault.pNext = probe.pNext;
                    probe.pNext = &supportedFault;
                }
                if (hasMeshShaderExtension)
                {
                    supportedMeshShader.pNext = probe.pNext;
                    probe.pNext = &supportedMeshShader;
                }
                // Chaining an extension's feature struct on a device that does
                // not advertise the extension is undefined, so each link is
                // gated on its own listing rather than on the group.
                if (hasAccelStructExtension)
                {
                    supportedAccelStruct.pNext = probe.pNext;
                    probe.pNext = &supportedAccelStruct;
                }
                if (hasRayQueryExtension)
                {
                    supportedRayQuery.pNext = probe.pNext;
                    probe.pNext = &supportedRayQuery;
                }
                if (hasRayPipelineExtension)
                {
                    supportedRayPipeline.pNext = probe.pNext;
                    probe.pNext = &supportedRayPipeline;
                }
                vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &probe);
                supportedFault.pNext = nullptr;
                supportedMeshShader.pNext = nullptr;
                // Scrub every reused struct: these are probe results now and
                // request structs later, and a stale pNext would splice the
                // probe chain into vkCreateDevice's.
                supportedAccelStruct.pNext = nullptr;
                supportedRayQuery.pNext = nullptr;
                supportedRayPipeline.pNext = nullptr;
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
            eds3Features.pNext = &vulkan14Features;
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
        // requires the feature (#691, same when-supported rule).
        enabledFeatures.multiDrawIndirect = supported.multiDrawIndirect;
        // Vertex-stage SSBO writes (ShaderDebugDraw's channel buffers) and
        // fragment-stage storage images (VirtualGeometry's debug images) are
        // rejected at pipeline creation without these two core features
        // (#691, when-supported rule as above).
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
        // gate row (#691).
        enabledFeatures.independentBlend = supported.independentBlend;
        m_TessellationShaderEnabled = supported.tessellationShader == VK_TRUE;
        m_GeometryShaderEnabled = supported.geometryShader == VK_TRUE;
        m_MultiDrawIndirectEnabled = supported.multiDrawIndirect == VK_TRUE;

        // shaderBufferInt64Atomics: the facade REPORTS this capability
        // (SupportsInt64ShaderAtomics feeds the virtual-geometry software
        // rasterizer's path choice), and a queried-but-not-enabled feature is
        // a shader crash waiting to happen. Enabled when supported,
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
        // #809: hostImageCopy + maintenance5 ride the same probe. Both are
        // required-to-be-supported on a 1.4 device, but "required by the spec"
        // is not "present in this driver" -- a device that reports 1.4 without
        // them, or a layer that filters them out, must degrade to the staging
        // path rather than call through a null volk pointer, so the probe
        // result is what gates every use.
        VkPhysicalDeviceVulkan14Features supported14{};
        supported14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
        supported14.pNext = &supported12;
        VkPhysicalDeviceFeatures2 supportedFeatures2{};
        supportedFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supportedFeatures2.pNext = &supported14;
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

        // #809. The member flags are committed AFTER vkCreateDevice returns
        // (the same commit-after-create rule the EDS3 / mesh-shader bits
        // follow): until the create succeeds nothing is enabled anywhere, and
        // a use site reading a flag set from a *request* would be reading a
        // wish. maintenance6 is deliberately NOT requested -- nothing in the
        // backend consumes any of its relaxations, and an unused feature bit
        // is dead weight the validation layer still has to reason about.
        const bool wantHostImageCopy = supported14.hostImageCopy == VK_TRUE;
        const bool wantMaintenance5 = supported14.maintenance5 == VK_TRUE;
        vulkan14Features.hostImageCopy = supported14.hostImageCopy;
        vulkan14Features.maintenance5 = supported14.maintenance5;

        // Device-fault reporting costs nothing until a device loss, at which
        // point it is the difference between "VkResult -4" and a fault
        // address — enable whenever the driver offers it.
        VkPhysicalDeviceFaultFeaturesEXT faultFeatures{};
        faultFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
        const bool wantDeviceFault = hasDeviceFaultExtension && supportedFault.deviceFault == VK_TRUE;
        if (wantDeviceFault)
        {
            deviceExtensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            faultFeatures.deviceFault = VK_TRUE;
            // deviceFaultVendorBinary deliberately left off: it gates a
            // vendor blob dump we have no decoder for.
        }

        // Mesh shaders (issue #813): OPTIONAL — enabled when the extension is
        // listed AND the driver supports both stages; never an ADR 0010 gate
        // row (the gate list must not widen — a device without mesh shaders
        // still satisfies the contract, and the facade routes mesh-pipeline
        // work away via SupportsMeshShaders). Only meshShader + taskShader
        // are enabled; multiview / primitiveFragmentShadingRate /
        // meshShaderQueries stay FALSE — feature bits nothing uses are dead
        // weight the validation layer still has to reason about (the EDS3
        // rule above). VkPhysicalDeviceMeshShaderFeaturesEXT is not
        // version-promoted, so it stays a standalone chained struct (no
        // VUID-VkDeviceCreateInfo-pNext-02830 fold into VulkanNNFeatures).
        VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
        meshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        const bool wantMeshShader = hasMeshShaderExtension &&
                                    supportedMeshShader.meshShader == VK_TRUE &&
                                    supportedMeshShader.taskShader == VK_TRUE;
        if (wantMeshShader)
        {
            deviceExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
            meshShaderFeatures.meshShader = VK_TRUE;
            meshShaderFeatures.taskShader = VK_TRUE;
        }

        // Hardware ray tracing (issue #978). OPTIONAL, never an ADR 0010 gate
        // row — the same Tier-2 rule as mesh shaders. The unsupported REASON
        // is decided here rather than at a use site, because this is the only
        // place that can tell "the extension is absent" from "the extension
        // is present and its feature bit is off", and #978 requires the
        // renderer to report which.
        //
        // Only the bits this subsystem consumes are enabled.
        // accelerationStructureCaptureReplay, accelerationStructureHostCommands
        // and accelerationStructureIndirectBuild stay FALSE: nothing uses them
        // and an unused feature bit is dead weight the validation layer still
        // has to reason about (the EDS3 / mesh-shader rule).
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures{};
        accelStructFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        rayQueryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayPipelineFeatures{};
        rayPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

        // OLO_VULKAN_NO_RAY_TRACING=1 forces the whole subsystem off so a
        // frame or validation difference can be attributed to it without a
        // rebuild — the OLO_VULKAN_NO_HOST_IMAGE_COPY genre, and the
        // "give every alternative GPU path a runtime lever" rule from the
        // mesh-shader port (rhi-abstraction-boundary.md §14c).
        const bool rayTracingForcedOff = Levers::VulkanNoRayTracing();
        const bool rtExtensionsListed = hasAccelStructExtension && hasDeferredHostOpsExtension && hasRayQueryExtension;
        const bool rtFeaturesSupported = supportedAccelStruct.accelerationStructure == VK_TRUE &&
                                         supportedRayQuery.rayQuery == VK_TRUE;
        const bool wantRayQuery = rtExtensionsListed && rtFeaturesSupported && !rayTracingForcedOff;
        // The pipeline path rides on top of ray query; it is never enabled on
        // its own, so a device offering pipelines without ray query reports
        // the ray-query reason rather than half-enabling.
        const bool wantRayPipeline =
            wantRayQuery && hasRayPipelineExtension && supportedRayPipeline.rayTracingPipeline == VK_TRUE;

        RayTracing::UnsupportedReason rayTracingReason = RayTracing::UnsupportedReason::None;
        if (rayTracingForcedOff)
        {
            rayTracingReason = RayTracing::UnsupportedReason::DisabledByLever;
        }
        else if (!rtExtensionsListed)
        {
            rayTracingReason = RayTracing::UnsupportedReason::ExtensionMissing;
        }
        else if (!rtFeaturesSupported)
        {
            rayTracingReason = RayTracing::UnsupportedReason::FeatureUnsupported;
        }

        if (wantRayQuery)
        {
            deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            accelStructFeatures.accelerationStructure = VK_TRUE;
            rayQueryFeatures.rayQuery = VK_TRUE;
        }
        if (wantRayPipeline)
        {
            deviceExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            rayPipelineFeatures.rayTracingPipeline = VK_TRUE;
        }

        void* featureChainHead = wantDynamicBlend ? static_cast<void*>(&eds3Features)
                                                  : static_cast<void*>(&vulkan14Features);
        if (wantDeviceFault)
        {
            faultFeatures.pNext = featureChainHead;
            featureChainHead = &faultFeatures;
        }
        if (wantMeshShader)
        {
            meshShaderFeatures.pNext = featureChainHead;
            featureChainHead = &meshShaderFeatures;
        }
        // None of the three RT feature structs is version-promoted, so each
        // stays a standalone chained struct (VUID-VkDeviceCreateInfo-pNext-02830
        // rejects a promoted feature spelled both ways at once).
        if (wantRayQuery)
        {
            accelStructFeatures.pNext = featureChainHead;
            featureChainHead = &accelStructFeatures;
            rayQueryFeatures.pNext = featureChainHead;
            featureChainHead = &rayQueryFeatures;
        }
        if (wantRayPipeline)
        {
            rayPipelineFeatures.pNext = featureChainHead;
            featureChainHead = &rayPipelineFeatures;
        }

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = featureChainHead;
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
        m_DeviceFaultEnabled = wantDeviceFault;
        // Same commit-after-create rule: the flag describes the LOGICAL
        // device's enabled features, which exist only once the create returns.
        m_MeshShaderEnabled = wantMeshShader;
        volkLoadDevice(m_Device);

        // Ray tracing, committed after volkLoadDevice for the reason
        // m_Maintenance5Enabled documents below: an enabled feature bit is not
        // proof the command is callable, because volk only populates a pointer
        // the loader/ICD actually exported. Every entry point this subsystem
        // calls is null-checked here, once, rather than at each call site.
        const bool rayTracingEntryPointsLoaded =
            vkCreateAccelerationStructureKHR != nullptr && vkDestroyAccelerationStructureKHR != nullptr &&
            vkGetAccelerationStructureBuildSizesKHR != nullptr && vkCmdBuildAccelerationStructuresKHR != nullptr &&
            vkGetAccelerationStructureDeviceAddressKHR != nullptr && vkCmdCopyAccelerationStructureKHR != nullptr &&
            vkCmdWriteAccelerationStructuresPropertiesKHR != nullptr;
        m_RayQueryEnabled = wantRayQuery && rayTracingEntryPointsLoaded;
        m_RayTracingPipelineEnabled = m_RayQueryEnabled && wantRayPipeline;
        m_RayTracingUnsupportedReason = m_RayQueryEnabled ? RayTracing::UnsupportedReason::None
                                        : (wantRayQuery && !rayTracingEntryPointsLoaded)
                                            ? RayTracing::UnsupportedReason::EntryPointMissing
                                            : rayTracingReason;
        if (m_RayQueryEnabled)
        {
            VkPhysicalDeviceAccelerationStructurePropertiesKHR accelProps{};
            accelProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR pipelineProps{};
            pipelineProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 rtProps2{};
            rtProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            rtProps2.pNext = &accelProps;
            if (m_RayTracingPipelineEnabled)
            {
                accelProps.pNext = &pipelineProps;
            }
            vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &rtProps2);
            accelProps.pNext = nullptr;

            m_RayTracingProperties.MaxGeometryCount = accelProps.maxGeometryCount;
            m_RayTracingProperties.MaxInstanceCount = accelProps.maxInstanceCount;
            m_RayTracingProperties.MaxPrimitiveCount = accelProps.maxPrimitiveCount;
            m_RayTracingProperties.MinScratchOffsetAlignment = accelProps.minAccelerationStructureScratchOffsetAlignment;
            if (m_RayTracingPipelineEnabled)
            {
                m_RayTracingProperties.ShaderGroupHandleSize = pipelineProps.shaderGroupHandleSize;
                m_RayTracingProperties.ShaderGroupBaseAlignment = pipelineProps.shaderGroupBaseAlignment;
                m_RayTracingProperties.ShaderGroupHandleAlignment = pipelineProps.shaderGroupHandleAlignment;
            }
            // Recorded, never gated on: these two are FALSE on the primary
            // development GPU, and the builder's device-only, non-indirect
            // path is correct either way. (Same discipline as
            // identicalMemoryTypeRequirements, which gating on once disabled a
            // whole feature here.)
            m_RayTracingProperties.SupportsHostCommands = supportedAccelStruct.accelerationStructureHostCommands == VK_TRUE;
            m_RayTracingProperties.SupportsIndirectBuild = supportedAccelStruct.accelerationStructureIndirectBuild == VK_TRUE;
        }
        if (rayTracingForcedOff)
        {
            OLO_CORE_WARN("[Vulkan] ray tracing disabled by OLO_VULKAN_NO_RAY_TRACING=1 — "
                          "the RT scene reports unsupported and the raster path is unaffected");
        }
        // Loud either way: a silent fallback here would make every later
        // measurement a measurement of the wrong path (§14a).
        if (m_RayQueryEnabled)
        {
            OLO_CORE_INFO("[Vulkan] Ray tracing: ray query enabled{} (scratch align {} B, max instances {}, "
                          "max primitives {})",
                          m_RayTracingPipelineEnabled ? " + ray-tracing pipeline" : "",
                          m_RayTracingProperties.MinScratchOffsetAlignment, m_RayTracingProperties.MaxInstanceCount,
                          m_RayTracingProperties.MaxPrimitiveCount);
        }
        else
        {
            OLO_CORE_INFO("[Vulkan] Ray tracing: unavailable ({}) — the RT scene stays empty and every "
                          "raster path is unaffected",
                          RayTracing::ToString(m_RayTracingUnsupportedReason));
        }
        vkGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_Queue);

        // #809: same commit-after-create rule, and the entry points these
        // flags gate only exist once volkLoadDevice has run -- a flag set
        // before that would let a caller dereference a null
        // vkCopyMemoryToImage. The null checks are not belt-and-braces: volk
        // only populates a core-1.4 pointer when the loader/ICD actually
        // exports it, so an enabled feature bit is not on its own proof the
        // command is callable.
        m_Maintenance5Enabled = wantMaintenance5 && vkCmdBindIndexBuffer2 != nullptr;
        // OLO_VULKAN_NO_HOST_IMAGE_COPY=1 forces every upload back onto the
        // staging + one-shot path. Same genre as
        // OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL: a one-line A/B for the question
        // "is this frame/validation difference the host route's fault?", which
        // is otherwise only answerable by rebuilding the backend. Kept
        // permanently because the host route changes WHEN an upload happens
        // relative to the queue, and that class of difference is exactly what
        // a bisect lever is for.
        // Declared in DebugLevers.inl and read through Levers, not through a
        // raw getenv or even a bare Env::IsTruthy: the registry is what makes
        // a lever discoverable (it carries the help text and shows up in the
        // snapshot), and DebugLeversTest fails the build for any engine code
        // that reads an OLO_* variable around it.
        const bool hostCopyForcedOff = Levers::VulkanNoHostImageCopy();
        m_HostImageCopyEnabled = wantHostImageCopy && !hostCopyForcedOff && vkCopyMemoryToImage != nullptr &&
                                 vkTransitionImageLayout != nullptr;
        if (hostCopyForcedOff)
        {
            OLO_CORE_WARN("[Vulkan] host image copy disabled by OLO_VULKAN_NO_HOST_IMAGE_COPY=1 — "
                          "every texture upload takes the staging + one-shot path");
        }
        if (m_HostImageCopyEnabled)
        {
            // The layout lists are a two-call query (count, then data). Only
            // VK_IMAGE_LAYOUT_GENERAL is guaranteed to appear in both, so
            // every host path that wants a nicer layout asks rather than
            // assumes -- see IsHostCopySrcLayoutSupported.
            VkPhysicalDeviceHostImageCopyProperties hostCopyProps{};
            hostCopyProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES;
            VkPhysicalDeviceProperties2 hostCopyProps2{};
            hostCopyProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            hostCopyProps2.pNext = &hostCopyProps;
            vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &hostCopyProps2);
            m_HostCopySrcLayouts.assign(hostCopyProps.copySrcLayoutCount, VK_IMAGE_LAYOUT_UNDEFINED);
            m_HostCopyDstLayouts.assign(hostCopyProps.copyDstLayoutCount, VK_IMAGE_LAYOUT_UNDEFINED);
            hostCopyProps.pCopySrcLayouts = m_HostCopySrcLayouts.empty() ? nullptr : m_HostCopySrcLayouts.data();
            hostCopyProps.pCopyDstLayouts = m_HostCopyDstLayouts.empty() ? nullptr : m_HostCopyDstLayouts.data();
            vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &hostCopyProps2);
            m_HostCopyMemoryTypeNeutral = hostCopyProps.identicalMemoryTypeRequirements == VK_TRUE;
        }
        OLO_CORE_INFO("[Vulkan] 1.4 conveniences: host image copy {}{}, maintenance5 {}",
                      m_HostImageCopyEnabled ? "enabled" : "unavailable (staging upload path)",
                      m_HostImageCopyEnabled && !m_HostCopyMemoryTypeNeutral
                          ? " (host-transfer usage changes memory type requirements — kept off render targets)"
                          : "",
                      m_Maintenance5Enabled ? "enabled" : "unavailable (implicit whole-buffer index binds)");

        // Mesh-shader limits + the loud capability verdict (issue #813). The
        // vkGetPhysicalDeviceProperties2 probe is this backend's first —
        // everything else gets by on plain vkGetPhysicalDeviceProperties,
        // which cannot chain extension structs — and runs only when the
        // extension is present (chaining an extension's properties struct on
        // a device that does not advertise it is undefined).
        if (hasMeshShaderExtension)
        {
            m_MeshShaderProperties = VkPhysicalDeviceMeshShaderPropertiesEXT{};
            m_MeshShaderProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &m_MeshShaderProperties;
            vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &props2);
            m_MeshShaderProperties.pNext = nullptr;
        }
        // The capability decision must be loudly observable either way — the
        // whole mesh-shader design is refuse-or-degrade, decided explicitly,
        // never silent (issue #813).
        if (m_MeshShaderEnabled)
        {
            OLO_CORE_INFO("[Vulkan] Mesh shaders: enabled (task+mesh; maxMeshOutputVertices={}, "
                          "maxMeshOutputPrimitives={}, maxPreferredMeshWorkGroupInvocations={})",
                          m_MeshShaderProperties.maxMeshOutputVertices,
                          m_MeshShaderProperties.maxMeshOutputPrimitives,
                          m_MeshShaderProperties.maxPreferredMeshWorkGroupInvocations);
        }
        else
        {
            OLO_CORE_INFO("[Vulkan] Mesh shaders: unavailable ({}) — DrawMeshTasks will drop loudly; "
                          "callers must route mesh-pipeline work to the classic path",
                          hasMeshShaderExtension ? "VK_EXT_mesh_shader present but taskShader/meshShader unsupported"
                                                 : "VK_EXT_mesh_shader not present");
        }

        // --- VMA (vendoring proof + the allocator the VMA-backed ---------------
        // transient resources allocate from, reached via VulkanDevice::Get())
        {
            VmaVulkanFunctions vulkanFunctions{};
            VmaAllocatorCreateInfo allocatorInfo{};
            allocatorInfo.physicalDevice = m_PhysicalDevice;
            allocatorInfo.device = m_Device;
            allocatorInfo.instance = m_Instance;
            allocatorInfo.vulkanApiVersion = VulkanCapabilities::kMinApiVersion;
            // bufferDeviceAddress is enabled above; without this flag
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
        // The original teardown, unchanged. The caller destroys its surface and
        // frame-loop objects BEFORE calling this (surface before instance).
        if (m_Device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(m_Device);
            // The parallel recorder's per-(slot, worker) pools belong to this
            // device (#806); every owner — the context, the test fixtures —
            // reaches this Shutdown, so this is the one release point.
            VulkanSecondaryCommandPools::Get().ReleaseAll();
            if (m_CommandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_Device, m_CommandPool, nullptr);
                m_CommandPool = VK_NULL_HANDLE;
            }
            if (m_Allocator != VK_NULL_HANDLE)
            {
                // vmaDestroyAllocator ASSERTS (debug-CRT abort in Debug) when
                // allocations are still alive — name the leaks first so the
                // abort is attributable instead of a bare VMA call stack.
                VmaTotalStatistics stats{};
                vmaCalculateStatistics(m_Allocator, &stats);
                if (stats.total.statistics.allocationCount > 0)
                {
                    OLO_CORE_ERROR("[Vulkan] {} VMA allocation(s) still alive at allocator teardown ({} bytes) — "
                                   "dumping detailed stats",
                                   stats.total.statistics.allocationCount, stats.total.statistics.allocationBytes);
                    char* statsString = nullptr;
                    vmaBuildStatsString(m_Allocator, &statsString, VK_TRUE);
                    if (statsString != nullptr)
                    {
                        OLO_CORE_ERROR("[Vulkan] VMA leak dump:\n{}", statsString);
                        vmaFreeStatsString(m_Allocator, statsString);
                    }
                }
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
        // #809: the host-image-copy verdict describes a physical device that
        // is no longer reachable through this object. Clearing it means a
        // Shutdown/Init cycle re-probes rather than reusing a verdict (and a
        // per-format memo) minted against the previous device.
        m_HostImageCopyEnabled = false;
        m_Maintenance5Enabled = false;
        m_HostCopySrcLayouts.clear();
        m_HostCopyDstLayouts.clear();
        m_HostCopyMemoryTypeNeutral = false;
        {
            std::lock_guard<std::mutex> lock(m_HostImageCopyFormatMutex);
            m_HostImageCopyFormatCache.clear();
        }
        if (s_ActiveDevice == this)
        {
            s_ActiveDevice = nullptr;
        }
    }

    bool VulkanDevice::SupportsHostImageCopyForFormat(VkFormat format) const
    {
        if (!m_HostImageCopyEnabled || m_PhysicalDevice == VK_NULL_HANDLE)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_HostImageCopyFormatMutex);
        if (const auto it = m_HostImageCopyFormatCache.find(format); it != m_HostImageCopyFormatCache.end())
        {
            return it->second;
        }

        // VkFormatProperties3, not VkFormatProperties: the host-transfer bit
        // lives above bit 31 and only the 64-bit flags word can carry it.
        VkFormatProperties3 properties3{};
        properties3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
        VkFormatProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        properties2.pNext = &properties3;
        vkGetPhysicalDeviceFormatProperties2(m_PhysicalDevice, format, &properties2);

        const bool supported = (properties3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT) != 0;
        m_HostImageCopyFormatCache.emplace(format, supported);
        return supported;
    }

    bool VulkanDevice::IsHostCopySrcLayoutSupported(VkImageLayout layout) const
    {
        if (!m_HostImageCopyEnabled)
        {
            return false;
        }
        return std::ranges::find(m_HostCopySrcLayouts, layout) != m_HostCopySrcLayouts.end();
    }

    bool VulkanDevice::IsHostCopyDstLayoutSupported(VkImageLayout layout) const
    {
        if (!m_HostImageCopyEnabled)
        {
            return false;
        }
        return std::ranges::find(m_HostCopyDstLayouts, layout) != m_HostCopyDstLayouts.end();
    }

    void VulkanDevice::LogDeviceFaultInfo() const
    {
        if (!m_DeviceFaultEnabled || m_Device == VK_NULL_HANDLE || vkGetDeviceFaultInfoEXT == nullptr)
        {
            return;
        }

        VkDeviceFaultCountsEXT counts{};
        counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        if (vkGetDeviceFaultInfoEXT(m_Device, &counts, nullptr) < VK_SUCCESS)
        {
            OLO_CORE_ERROR("[Vulkan] device fault: vkGetDeviceFaultInfoEXT(counts) itself failed");
            return;
        }

        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
        VkDeviceFaultInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
        info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
        counts.vendorBinarySize = 0; // deviceFaultVendorBinary is not enabled
        if (vkGetDeviceFaultInfoEXT(m_Device, &counts, &info) < VK_SUCCESS)
        {
            OLO_CORE_ERROR("[Vulkan] device fault: vkGetDeviceFaultInfoEXT(info) itself failed");
            return;
        }

        OLO_CORE_ERROR("[Vulkan] DEVICE FAULT REPORT: '{}' ({} address record(s), {} vendor record(s))",
                       info.description, counts.addressInfoCount, counts.vendorInfoCount);
        // The info call updates the counts to what it actually WROTE (it may
        // return VK_INCOMPLETE with fewer records than the first call
        // promised) — iterate only the written prefix, never the full
        // first-call-sized vectors (review finding).
        addresses.resize(std::min<sizet>(addresses.size(), counts.addressInfoCount));
        vendors.resize(std::min<sizet>(vendors.size(), counts.vendorInfoCount));
        for (const auto& a : addresses)
        {
            const char* type = "?";
            switch (a.addressType)
            {
                case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
                    type = "none";
                    break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
                    type = "READ of invalid address";
                    break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
                    type = "WRITE to invalid address";
                    break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
                    type = "EXECUTE of invalid address";
                    break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
                    type = "instruction pointer (unknown)";
                    break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
                    type = "instruction pointer (invalid)";
                    break;
                case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
                    type = "instruction pointer (faulting)";
                    break;
                default:
                    break;
            }
            // reportedAddress is only precise to addressPrecision (a power of
            // two) — log the ±window so it can be matched against buffer
            // device addresses.
            OLO_CORE_ERROR("[Vulkan]   fault address: {:#x} (precision ±{:#x}) — {}",
                           static_cast<u64>(a.reportedAddress), static_cast<u64>(a.addressPrecision), type);
        }
        for (const auto& v : vendors)
        {
            OLO_CORE_ERROR("[Vulkan]   vendor fault: '{}' code={:#x} data={:#x}", v.description,
                           static_cast<u64>(v.vendorFaultCode), static_cast<u64>(v.vendorFaultData));
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
