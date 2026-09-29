# Vulkan capability report

Produced by the engine's own device-selection reader, `VulkanCapabilities::Evaluate`, and the real
gate, `VulkanDevice::Init` (headless). Nothing below is inferred from a vendor name.

## Environment

- OS: Windows
- Toolchain: MSVC 1951, Release/RelWithDebInfo
- Vulkan headers the engine was built against: 1.4.357
- Vulkan SDK floor (ADR 0010 tooling floor): 1.4.357.0
- Loader instance version: 1.4.357

## Devices (1)

### Device 0: llvmpipe (LLVM 22.1.8, 256 bits) - SOFTWARE (not a hardware vendor)

- Driver: llvmpipe Mesa 26.2.0 (git-aacd123e02) (LLVM 22.1.8) (driverVersion 26.2.0), vendorID 0x10005
- API version: 1.4.354
- **ADR 0010 contract: SATISFIED**

| Requirement | Met |
| --- | --- |
| `Vulkan API version 1.4` | yes |
| `VK_KHR_swapchain` | yes |
| `VK_EXT_descriptor_heap` | yes |
| `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` | yes |
| `VK_KHR_shader_untyped_pointers` | yes |
| `VkPhysicalDeviceShaderUntypedPointersFeaturesKHR::shaderUntypedPointers` | yes |
| `VK_KHR_device_address_commands` | yes |
| `VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR::deviceAddressCommands` | yes |

Optional (never part of the verdict): VK_KHR_unified_image_layouts usable; ray-query extensions listed, accelerationStructure+rayQuery features true.

Satisfying devices: **0 hardware**, 1 software (software is reported, never counted as another hardware vendor).

## The real gate

`VulkanDevice::Init` accepted **llvmpipe (LLVM 22.1.8, 256 bits)**. Ray query enabled: **yes**.

## Tests this machine can execute

| Population | Filter | Executable | Why |
| --- | --- | --- | --- |
| Device-gated Vulkan suites (OLO_VULKAN_DEVICE_OR_SKIP) | `Vulkan*:VirtualShadowMapVulkanShaders*` | **yes** | only a SOFTWARE device satisfies the ADR 0010 contract |
| L7 ray-query device suites (#1294) | `ReSTIRPTDevice.*:RayTracingDevice.*:GpuPathTracerDevice.*` | **yes** | the gate's device enabled ray query (IsRayQueryEnabled) |

A skipped test is not a passing test. To make a designated run fail instead of skip, pass `--olo-require-vulkan` (issue #1300); the rows above say whether that run could succeed here.
