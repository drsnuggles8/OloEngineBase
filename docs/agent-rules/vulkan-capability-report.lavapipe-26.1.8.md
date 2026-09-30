# Vulkan capability report

Produced by the engine's own device-selection reader, `VulkanCapabilities::Evaluate`, and the real
gate, `VulkanDevice::Init` (headless). Nothing below is inferred from a vendor name.

## Environment

- OS: Windows
- Toolchain: clang 23.1.0 (https://github.com/llvm/llvm-project ea7d852a70e8bdfaf601d6626a760f9771b2c4b4), Debug
- Vulkan headers the engine was built against: 1.4.357
- Vulkan SDK floor (ADR 0010 tooling floor): 1.4.357.0
- Loader instance version: 1.4.357

## Devices (1)

### Device 0: llvmpipe (LLVM 23.1.1, 256 bits) - SOFTWARE (not a hardware vendor)

- Driver: llvmpipe Mesa 26.1.8 (git-c5ec97122e) (LLVM 23.1.1) (driverVersion 26.1.8), vendorID 0x10005
- API version: 1.4.354
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

Optional (never part of the verdict): VK_KHR_unified_image_layouts usable; ray-query extensions listed, accelerationStructure+rayQuery features true.

Satisfying devices: **0 hardware**, 0 software (software is reported, never counted as another hardware vendor).

## The real gate

`VulkanDevice::Init` **refused**: Vulkan bring-up: no device satisfies the capability contract (ADR 0010). Closest device 'llvmpipe (LLVM 23.1.1, 256 bits)' is missing: VK_EXT_descriptor_heap, VK_KHR_shader_untyped_pointers, VK_KHR_device_address_commands. Run with --rhi=opengl.

## Tests this machine can execute

| Population | Filter | Executable | Why |
| --- | --- | --- | --- |
| Device-gated Vulkan suites (OLO_VULKAN_DEVICE_OR_SKIP) | `Vulkan*:VirtualShadowMapVulkanShaders*` | no - skips | no device satisfies the ADR 0010 contract; every such test skips |
| L7 ray-query device suites (#1294) | `ReSTIRPTDevice.*:RayTracingDevice.*:GpuPathTracerDevice.*` | no - skips | no contract-satisfying device, so the device suites never start |

A skipped test is not a passing test. To make a designated run fail instead of skip, pass `--olo-require-vulkan` (issue #1300); the rows above say whether that run could succeed here.
