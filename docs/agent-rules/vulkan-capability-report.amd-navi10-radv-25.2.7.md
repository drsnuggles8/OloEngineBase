# Vulkan capability report

Produced by the engine's own device-selection reader, `VulkanCapabilities::Evaluate`, and the real
gate, `VulkanDevice::Init` (headless). Nothing below is inferred from a vendor name.

## Environment

- OS: Linux
- Toolchain: gcc 14.3.1 20251022 (Red Hat 14.3.1-4), Release/RelWithDebInfo
- Vulkan headers the engine was built against: 1.4.357
- Vulkan SDK floor (ADR 0010 tooling floor): 1.4.357.0
- Loader instance version: 1.4.328

## Devices (2)

### Device 0: AMD Radeon RX 5600 XT (RADV NAVI10) - hardware, discrete

- Driver: radv Mesa 25.2.7 (driverVersion 25.2.7), vendorID 0x1002
- API version: 1.4.318
- **ADR 0010 contract: NOT satisfied** - missing: VK_EXT_descriptor_heap, VK_KHR_shader_untyped_pointers, VK_KHR_device_address_commands

| Requirement | Met |
| --- | --- |
| `Vulkan API version 1.4` | yes |
| `VK_KHR_swapchain` | yes |
| `VK_EXT_descriptor_heap` | **no** |
| `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` | **no** |
| `VK_KHR_shader_untyped_pointers` | **no** |
| `VkPhysicalDeviceShaderUntypedPointersFeaturesKHR::shaderUntypedPointers` | **no** |
| `VK_KHR_device_address_commands` | **no** |
| `VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR::deviceAddressCommands` | **no** |

Optional (never part of the verdict): VK_KHR_unified_image_layouts not usable; ray-query extensions not listed, accelerationStructure+rayQuery features false.

### Device 1: llvmpipe (LLVM 21.1.8, 256 bits) - SOFTWARE (not a hardware vendor)

- Driver: llvmpipe Mesa 25.2.7 (LLVM 21.1.8) (driverVersion 25.2.7), vendorID 0x10005
- API version: 1.4.318
- **ADR 0010 contract: NOT satisfied** - missing: VK_EXT_descriptor_heap, VK_KHR_shader_untyped_pointers, VK_KHR_device_address_commands

| Requirement | Met |
| --- | --- |
| `Vulkan API version 1.4` | yes |
| `VK_KHR_swapchain` | yes |
| `VK_EXT_descriptor_heap` | **no** |
| `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` | **no** |
| `VK_KHR_shader_untyped_pointers` | **no** |
| `VkPhysicalDeviceShaderUntypedPointersFeaturesKHR::shaderUntypedPointers` | **no** |
| `VK_KHR_device_address_commands` | **no** |
| `VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR::deviceAddressCommands` | **no** |

Optional (never part of the verdict): VK_KHR_unified_image_layouts not usable; ray-query extensions listed, accelerationStructure+rayQuery features true.

Satisfying devices: **0 hardware**, 0 software (software is reported, never counted as another hardware vendor).

## The real gate

`VulkanDevice::Init` **refused**: Vulkan bring-up: no device satisfies the capability contract (ADR 0010). Closest device 'AMD Radeon RX 5600 XT (RADV NAVI10)' is missing: VK_EXT_descriptor_heap, VK_KHR_shader_untyped_pointers, VK_KHR_device_address_commands. Run with --rhi=opengl.

## Tests this machine can execute

| Population | Filter | Executable | Why |
| --- | --- | --- | --- |
| Device-gated Vulkan suites (OLO_VULKAN_DEVICE_OR_SKIP) | `Vulkan*:VirtualShadowMapVulkanShaders*` | no - skips | no device satisfies the ADR 0010 contract; every such test skips |
| L7 ray-query device suites (#1294) | `ReSTIRPTDevice.*:RayTracingDevice.*:GpuPathTracerDevice.*` | no - skips | no contract-satisfying device, so the device suites never start |

A skipped test is not a passing test. To make a designated run fail instead of skip, pass `--olo-require-vulkan` (issue #1300); the rows above say whether that run could succeed here.
