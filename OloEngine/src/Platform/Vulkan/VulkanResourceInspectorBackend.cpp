#include "OloEnginePCH.h"
#include "Platform/Vulkan/VulkanResourceInspectorBackend.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanStorageBuffer.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // The engine's own format set (VulkanBarrierLowering::ToVkFormat) plus
        // the depth flavours the allocator actually creates. Anything else
        // prints as a hex enum value rather than being guessed at — a wrong
        // format name on a diagnostic is worse than a raw number a reader can
        // look up.
        std::string VkFormatName(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_UNDEFINED:
                    return "UNDEFINED";
                case VK_FORMAT_R8_UNORM:
                    return "R8_UNORM";
                case VK_FORMAT_R8_UINT:
                    return "R8_UINT";
                case VK_FORMAT_R8G8_UNORM:
                    return "R8G8_UNORM";
                case VK_FORMAT_R8G8B8A8_UNORM:
                    return "R8G8B8A8_UNORM";
                case VK_FORMAT_R8G8B8A8_SRGB:
                    return "R8G8B8A8_SRGB";
                case VK_FORMAT_B8G8R8A8_UNORM:
                    return "B8G8R8A8_UNORM";
                case VK_FORMAT_B8G8R8A8_SRGB:
                    return "B8G8R8A8_SRGB";
                case VK_FORMAT_R16_UINT:
                    return "R16_UINT";
                case VK_FORMAT_R16_SFLOAT:
                    return "R16_SFLOAT";
                case VK_FORMAT_R16G16_UINT:
                    return "R16G16_UINT";
                case VK_FORMAT_R16G16_SFLOAT:
                    return "R16G16_SFLOAT";
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                    return "R16G16B16A16_SFLOAT";
                case VK_FORMAT_R32_SFLOAT:
                    return "R32_SFLOAT";
                case VK_FORMAT_R32_SINT:
                    return "R32_SINT";
                case VK_FORMAT_R32_UINT:
                    return "R32_UINT";
                case VK_FORMAT_R32G32_SFLOAT:
                    return "R32G32_SFLOAT";
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                    return "R32G32B32A32_SFLOAT";
                case VK_FORMAT_R32G32B32A32_UINT:
                    return "R32G32B32A32_UINT";
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                    return "B10G11R11_UFLOAT_PACK32";
                case VK_FORMAT_D32_SFLOAT:
                    return "D32_SFLOAT";
                case VK_FORMAT_D32_SFLOAT_S8_UINT:
                    return "D32_SFLOAT_S8_UINT";
                case VK_FORMAT_D24_UNORM_S8_UINT:
                    return "D24_UNORM_S8_UINT";
                case VK_FORMAT_D16_UNORM:
                    return "D16_UNORM";
                case VK_FORMAT_BC5_UNORM_BLOCK:
                    return "BC5_UNORM_BLOCK";
                case VK_FORMAT_BC6H_UFLOAT_BLOCK:
                    return "BC6H_UFLOAT_BLOCK";
                case VK_FORMAT_BC7_UNORM_BLOCK:
                    return "BC7_UNORM_BLOCK";
                case VK_FORMAT_BC7_SRGB_BLOCK:
                    return "BC7_SRGB_BLOCK";
                default:
                    break;
            }
            return std::format("VkFormat(0x{:X})", static_cast<u32>(format));
        }

        // Bytes per texel for the uncompressed formats above; 0 = "do not
        // know", which the caller reports as an unknown size rather than
        // multiplying by a guess. Block-compressed formats return 0 too: their
        // footprint is per-block, and a per-texel product would be wrong by a
        // factor of 4-8 in the direction that makes a memory report look safe.
        u32 BytesPerTexel(VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_R8_UNORM:
                case VK_FORMAT_R8_UINT:
                    return 1u;
                case VK_FORMAT_R8G8_UNORM:
                case VK_FORMAT_R16_UINT:
                case VK_FORMAT_R16_SFLOAT:
                case VK_FORMAT_D16_UNORM:
                    return 2u;
                case VK_FORMAT_R8G8B8A8_UNORM:
                case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_B8G8R8A8_UNORM:
                case VK_FORMAT_B8G8R8A8_SRGB:
                case VK_FORMAT_R16G16_UINT:
                case VK_FORMAT_R16G16_SFLOAT:
                case VK_FORMAT_R32_SFLOAT:
                case VK_FORMAT_R32_SINT:
                case VK_FORMAT_R32_UINT:
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                case VK_FORMAT_D32_SFLOAT:
                case VK_FORMAT_D24_UNORM_S8_UINT:
                    return 4u;
                case VK_FORMAT_D32_SFLOAT_S8_UINT:
                    return 5u; // 4 depth + 1 stencil, planar in practice
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                case VK_FORMAT_R32G32_SFLOAT:
                    return 8u;
                case VK_FORMAT_R32G32B32A32_SFLOAT:
                case VK_FORMAT_R32G32B32A32_UINT:
                    return 16u;
                default:
                    break;
            }
            return 0u;
        }

        // Whole-chain footprint estimate for an image. Deliberately an
        // ESTIMATE, and named as one wherever it surfaces: it ignores tiling
        // padding and alignment, so the device-heap table (VMA's own numbers)
        // is the authority on "how much VRAM is gone".
        u64 EstimateImageBytes(const VulkanImageInfo& info)
        {
            const u32 bpp = BytesPerTexel(info.Format);
            if (bpp == 0u || info.Width == 0u || info.Height == 0u)
                return 0u;

            u64 total = 0u;
            for (u32 mip = 0u; mip < std::max(info.MipLevels, 1u); ++mip)
            {
                const u64 w = std::max(info.Width >> mip, 1u);
                const u64 h = std::max(info.Height >> mip, 1u);
                total += w * h * bpp;
            }
            return total * std::max(info.ArrayLayers, 1u);
        }

        const char* ViewTypeName(VkImageViewType type)
        {
            switch (type)
            {
                case VK_IMAGE_VIEW_TYPE_1D:
                    return "1D";
                case VK_IMAGE_VIEW_TYPE_2D:
                    return "2D";
                case VK_IMAGE_VIEW_TYPE_3D:
                    return "3D";
                case VK_IMAGE_VIEW_TYPE_CUBE:
                    return "Cube";
                case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
                    return "1DArray";
                case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
                    return "2DArray";
                case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
                    return "CubeArray";
                default:
                    break;
            }
            return "Unknown";
        }

        // The interface's native-target slot carries a VulkanRootObjectKind
        // here — Vulkan has no GL_ARRAY_BUFFER-style binding-point enum to put
        // in it. BIASED BY ONE so that 0 keeps its "nothing was recorded"
        // meaning: DiscoveredResource::NativeTarget defaults to 0, and
        // VulkanRootObjectKind::UniformBuffer is 0, so an unbiased encoding
        // makes every raw buffer and every size-unknown buffer classify and
        // display as "UniformBuffer (frame arena)" — a confident wrong answer
        // for a resource nothing could describe.
        [[nodiscard]] constexpr u32 EncodeRootKind(VulkanRootObjectKind kind)
        {
            return static_cast<u32>(kind) + 1u;
        }

        // Null when `nativeTarget` is the reserved 0, or past the last
        // enumerator. The upper bound names Framebuffer because it IS last;
        // a kind appended after it would decode to nothing and display as
        // unclassified, which degrades in the safe direction rather than
        // casting to an invalid enumerator.
        [[nodiscard]] constexpr std::optional<VulkanRootObjectKind> DecodeRootKind(u32 nativeTarget)
        {
            if (nativeTarget == 0u || nativeTarget > EncodeRootKind(VulkanRootObjectKind::Framebuffer))
                return std::nullopt;
            return static_cast<VulkanRootObjectKind>(nativeTarget - 1u);
        }

        // The root-object entry for one buffer-family handle, if any, plus the
        // size it can answer. `outTarget` re-uses the interface's native-target
        // slot to carry the ROOT KIND, which is what
        // ClassifyBufferTarget/GetBufferTargetName decode below — Vulkan has no
        // GL_ARRAY_BUFFER-style binding-point enum to put there.
        bool DescribeBufferFromRootRegistry(RHI::ResourceHandle handle, u64& outSize, u32& outTarget,
                                            std::string& outName)
        {
            const auto* entry = VulkanRootObjectRegistry::Get().Lookup(handle);
            if (entry == nullptr || entry->Object == nullptr)
                return false;

            outTarget = EncodeRootKind(entry->Kind);
            switch (entry->Kind)
            {
                case VulkanRootObjectKind::UniformBuffer:
                {
                    const auto* buffer = static_cast<const VulkanUniformBuffer*>(entry->Object);
                    outSize = buffer->GetAllocatedSize();
                    outName = std::format("UniformBuffer (binding {})", buffer->GetBinding());
                    return true;
                }
                case VulkanRootObjectKind::StorageBuffer:
                {
                    const auto* buffer = static_cast<const VulkanStorageBuffer*>(entry->Object);
                    outSize = buffer->GetSize();
                    outName = "StorageBuffer";
                    return true;
                }
                case VulkanRootObjectKind::VertexBuffer:
                {
                    const auto* buffer = static_cast<const VulkanVertexBuffer*>(entry->Object);
                    outSize = buffer->GetSize();
                    outName = "VertexBuffer";
                    return true;
                }
                case VulkanRootObjectKind::IndexBuffer:
                {
                    const auto* buffer = static_cast<const VulkanIndexBuffer*>(entry->Object);
                    outSize = static_cast<u64>(buffer->GetCount()) * sizeof(u32);
                    outName = std::format("IndexBuffer ({} indices)", buffer->GetCount());
                    return true;
                }
                case VulkanRootObjectKind::VertexArray:
                case VulkanRootObjectKind::Shader:
                case VulkanRootObjectKind::Framebuffer:
                    break;
            }
            return false;
        }
    } // namespace

    // ---- Registry-driven discovery -----------------------------------------

    void VulkanResourceInspectorBackend::DiscoverResources(std::vector<DiscoveredResource>& out)
    {
        out.clear();

        const auto snapshot = RHI::ResourceRegistry::Get().Snapshot();
        out.reserve(snapshot.size());

        for (const auto& slot : snapshot)
        {
            // A GL-owned entry can legitimately coexist in the same process
            // (the registry is process-wide and a test may have minted one);
            // describing it from Vulkan side tables would invent an answer.
            if (slot.Owner != RHI::Backend::Vulkan)
                continue;

            DiscoveredResource entry;
            entry.Handle = slot.Handle;
            entry.Native = slot.Native;
            entry.Kind = slot.Kind;

            switch (slot.Kind)
            {
                case RHI::ResourceKind::Texture:
                {
                    const auto* info =
                        VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(slot.Native));
                    if (info == nullptr)
                    {
                        // Registered as a texture but absent from the image
                        // registry: a real inconsistency worth SEEING rather
                        // than dropping, so it lands as a row with no extent.
                        entry.Name = std::format("VkImage 0x{:X} (no image-info entry)", slot.Native);
                        entry.FormatName = "unknown";
                        break;
                    }
                    entry.IsCubemap = info->ViewType == VK_IMAGE_VIEW_TYPE_CUBE ||
                                      info->ViewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                    entry.Width = info->Width;
                    entry.Height = info->Height;
                    entry.MipLevels = std::max(info->MipLevels, 1u);
                    entry.ArrayLayers = std::max(info->ArrayLayers, 1u);
                    entry.NativeFormat = static_cast<u32>(info->Format);
                    entry.FormatName = VkFormatName(info->Format);
                    entry.SizeBytes = EstimateImageBytes(*info);
                    entry.Name = std::format("{} {}x{} {}", ViewTypeName(info->ViewType), info->Width,
                                             info->Height, entry.FormatName);
                    entry.DebugName = std::format("mips {}, layers {}{}{}", entry.MipLevels, entry.ArrayLayers,
                                                  info->HasDepth ? ", depth" : "",
                                                  info->HasStencil ? "+stencil" : "");
                    break;
                }
                case RHI::ResourceKind::Framebuffer:
                {
                    // Native is 0 here by design (no VkFramebuffer under
                    // dynamic rendering), so the object hop is the only route.
                    const auto* rootEntry = VulkanRootObjectRegistry::Get().Lookup(slot.Handle);
                    if (rootEntry != nullptr && rootEntry->Kind == VulkanRootObjectKind::Framebuffer &&
                        rootEntry->Object != nullptr)
                    {
                        const auto* framebuffer = static_cast<const VulkanFramebuffer*>(rootEntry->Object);
                        const auto& spec = framebuffer->GetSpecification();
                        entry.Width = spec.Width;
                        entry.Height = spec.Height;
                        entry.Name = std::format("Framebuffer {}x{}", spec.Width, spec.Height);
                    }
                    else
                    {
                        entry.Name = "Framebuffer (no root-object entry)";
                    }
                    entry.DebugName = "no VkFramebuffer — dynamic rendering";
                    break;
                }
                case RHI::ResourceKind::Buffer:
                {
                    u64 size = 0u;
                    u32 target = 0u;
                    std::string name;
                    if (DescribeBufferFromRootRegistry(slot.Handle, size, target, name))
                    {
                        entry.SizeBytes = size;
                        entry.NativeTarget = target;
                        entry.Name = std::move(name);
                    }
                    else if (const auto* raw = VulkanRawBufferRegistry::Get().Lookup(slot.Handle); raw != nullptr)
                    {
                        entry.SizeBytes = raw->Size;
                        entry.Name = "Raw buffer";
                        entry.DebugName = raw->Mapped != nullptr ? "host-visible, mapped" : "device-local";
                    }
                    else
                    {
                        // No object and no raw entry: size genuinely unknown.
                        // Say so rather than reporting 0 bytes as a fact.
                        entry.Name = std::format("VkBuffer 0x{:X} (size unknown)", slot.Native);
                    }
                    break;
                }
                case RHI::ResourceKind::VertexArray:
                {
                    entry.Name = "VertexArray";
                    entry.DebugName = "CPU-side stream layout — no native object";
                    break;
                }
                case RHI::ResourceKind::ShaderProgram:
                {
                    const auto* rootEntry = VulkanRootObjectRegistry::Get().Lookup(slot.Handle);
                    if (rootEntry != nullptr && rootEntry->Kind == VulkanRootObjectKind::Shader &&
                        rootEntry->Object != nullptr)
                    {
                        entry.Name = static_cast<const VulkanShader*>(rootEntry->Object)->GetName();
                    }
                    if (entry.Name.empty())
                        entry.Name = "Shader";
                    break;
                }
                case RHI::ResourceKind::Query:
                {
                    entry.Name = std::format("Query 0x{:X}", slot.Native);
                    break;
                }
                case RHI::ResourceKind::Unknown:
                {
                    entry.Name = std::format("Unknown 0x{:X}", slot.Native);
                    break;
                }
            }

            out.push_back(std::move(entry));
        }
    }

    bool VulkanResourceInspectorBackend::QueryMemoryHeaps(std::vector<MemoryHeap>& out)
    {
        out.clear();

        auto* device = VulkanDevice::Get();
        if (device == nullptr || device->GetAllocator() == VK_NULL_HANDLE)
            return false;

        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(device->GetAllocator(), budgets.data());

        const VkPhysicalDeviceMemoryProperties* memoryProperties = nullptr;
        vmaGetMemoryProperties(device->GetAllocator(), &memoryProperties);
        const u32 heapCount = memoryProperties != nullptr ? memoryProperties->memoryHeapCount : 0u;
        if (heapCount == 0u)
            return false;

        out.reserve(heapCount);
        for (u32 index = 0u; index < heapCount && index < budgets.size(); ++index)
        {
            MemoryHeap heap;
            heap.Index = index;
            heap.DeviceLocal =
                (memoryProperties->memoryHeaps[index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u;
            heap.UsageBytes = budgets[index].usage;
            heap.BudgetBytes = budgets[index].budget;
            heap.BlockBytes = budgets[index].statistics.blockBytes;
            heap.AllocationCount = budgets[index].statistics.allocationCount;
            heap.BlockCount = budgets[index].statistics.blockCount;
            out.push_back(heap);
        }
        return !out.empty();
    }

    // ---- Introspection ------------------------------------------------------

    void VulkanResourceInspectorBackend::QueryTexture(u64 nativeTextureId, bool /*isCubemap*/,
                                                      TextureQuery& outInfo)
    {
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(nativeTextureId));
        if (info == nullptr)
            return;

        outInfo.Width = info->Width;
        outInfo.Height = info->Height;
        outInfo.MipLevels = std::max(info->MipLevels, 1u);
        outInfo.HasMips = outInfo.MipLevels > 1u;
        // One VkFormat answers all three GL questions (internal format, pixel
        // format, data type are a GL decomposition Vulkan does not have), so
        // all three carry it and the decoders below read it as one value.
        outInfo.InternalFormat = static_cast<u32>(info->Format);
        outInfo.PixelFormat = static_cast<u32>(info->Format);
        outInfo.DataType = static_cast<u32>(info->Format);
        outInfo.MemoryUsage = static_cast<sizet>(EstimateImageBytes(*info));
    }

    void VulkanResourceInspectorBackend::QueryBuffer(u64 /*nativeBufferId*/, u32 nativeTarget,
                                                     BufferQuery& outInfo)
    {
        // Sizes come from the object hop in DiscoverResources (a VkBuffer alone
        // cannot be asked how big it is), so this fills only what the native
        // handle can honestly answer.
        outInfo.Usage = nativeTarget;
    }

    void VulkanResourceInspectorBackend::QueryFramebuffer(u64 /*nativeFramebufferId*/, FramebufferQuery& outInfo)
    {
        // Same: a Vulkan framebuffer identity resolves to native 0, so the
        // extent arrives through DiscoverResources' object hop.
        outInfo.Status = 0u;
    }

    IResourceInspectorBackend::BufferKind VulkanResourceInspectorBackend::ClassifyBufferTarget(u32 nativeTarget) const
    {
        // `nativeTarget` carries a BIASED VulkanRootObjectKind here — see
        // EncodeRootKind. An undescribed buffer decodes to nothing and falls
        // through to the Vertex default, which is the same fallback the shell
        // has always used for an unclassifiable buffer.
        if (const auto kind = DecodeRootKind(nativeTarget))
        {
            switch (*kind)
            {
                case VulkanRootObjectKind::IndexBuffer:
                    return BufferKind::Index;
                case VulkanRootObjectKind::UniformBuffer:
                case VulkanRootObjectKind::StorageBuffer:
                    return BufferKind::Uniform;
                default:
                    break;
            }
        }
        return BufferKind::Vertex;
    }

    u64 VulkanResourceInspectorBackend::GetBoundTexture2D() const
    {
        // There is no "currently bound 2D texture" under heap-bindless: every
        // sampled image is reachable through the descriptor heap for the whole
        // frame. 0 means "nothing bound", which is the honest answer and what
        // the shell already treats as no-op.
        return 0u;
    }

    void VulkanResourceInspectorBackend::GetTextureLevelSize(u64 nativeTextureId, u32 mipLevel, u32& outWidth,
                                                             u32& outHeight)
    {
        outWidth = 0u;
        outHeight = 0u;
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(reinterpret_cast<VkImage>(nativeTextureId));
        if (info == nullptr || info->Width == 0u || mipLevel >= std::max(info->MipLevels, 1u))
            return;
        outWidth = std::max(info->Width >> mipLevel, 1u);
        outHeight = std::max(info->Height >> mipLevel, 1u);
    }

    // ---- Native-enum vocabulary ---------------------------------------------

    i32 VulkanResourceInspectorBackend::ChannelCountForPixelFormat(u32 nativePixelFormat) const
    {
        switch (static_cast<VkFormat>(nativePixelFormat))
        {
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8_UINT:
            case VK_FORMAT_R16_UINT:
            case VK_FORMAT_R16_SFLOAT:
            case VK_FORMAT_R32_SFLOAT:
            case VK_FORMAT_R32_SINT:
            case VK_FORMAT_R32_UINT:
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return 1;
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R16G16_UINT:
            case VK_FORMAT_R16G16_SFLOAT:
            case VK_FORMAT_R32G32_SFLOAT:
                return 2;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
                return 3;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_R32G32B32A32_SFLOAT:
            case VK_FORMAT_R32G32B32A32_UINT:
                return 4;
            default:
                break;
        }
        return 0;
    }

    IResourceInspectorBackend::PixelPrecision
    VulkanResourceInspectorBackend::ClassifyPixelDataType(u32 nativeDataType) const
    {
        switch (static_cast<VkFormat>(nativeDataType))
        {
            case VK_FORMAT_R8_UNORM:
            case VK_FORMAT_R8G8_UNORM:
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                return PixelPrecision::UnsignedByte;
            case VK_FORMAT_R16_SFLOAT:
            case VK_FORMAT_R16G16_SFLOAT:
            case VK_FORMAT_R16G16B16A16_SFLOAT:
            case VK_FORMAT_R32_SFLOAT:
            case VK_FORMAT_R32G32_SFLOAT:
            case VK_FORMAT_R32G32B32A32_SFLOAT:
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            case VK_FORMAT_D32_SFLOAT:
                return PixelPrecision::Float;
            default:
                break;
        }
        // Integer and packed depth/stencil formats, same exclusion the GL arm
        // makes: a preview cannot show them meaningfully.
        return PixelPrecision::Unsupported;
    }

    bool VulkanResourceInspectorBackend::IsFloatPixelDataType(u32 nativeDataType) const
    {
        return ClassifyPixelDataType(nativeDataType) == PixelPrecision::Float;
    }

    std::string VulkanResourceInspectorBackend::FormatTextureFormatName(u32 nativeInternalFormat) const
    {
        return VkFormatName(static_cast<VkFormat>(nativeInternalFormat));
    }

    std::string VulkanResourceInspectorBackend::FormatBufferUsageName(u32 nativeUsage) const
    {
        return GetBufferTargetName(nativeUsage);
    }

    const char* VulkanResourceInspectorBackend::GetBufferTargetName(u32 nativeTarget) const
    {
        const auto kind = DecodeRootKind(nativeTarget);
        if (!kind)
        {
            // Nothing described this buffer (a raw-registry entry, or one whose
            // size could not be resolved). Say so rather than naming a family.
            return "Buffer (unclassified)";
        }
        switch (*kind)
        {
            case VulkanRootObjectKind::UniformBuffer:
                return "UniformBuffer (frame arena)";
            case VulkanRootObjectKind::StorageBuffer:
                return "StorageBuffer";
            case VulkanRootObjectKind::VertexBuffer:
                return "VertexBuffer";
            case VulkanRootObjectKind::IndexBuffer:
                return "IndexBuffer";
            case VulkanRootObjectKind::VertexArray:
                return "VertexArray";
            case VulkanRootObjectKind::Shader:
                return "Shader";
            case VulkanRootObjectKind::Framebuffer:
                return "Framebuffer";
        }
        return "Buffer";
    }

    const char* VulkanResourceInspectorBackend::FramebufferStatusName(u32 /*nativeStatus*/,
                                                                      FramebufferStatusClass& outClass) const
    {
        // Vulkan has no glCheckFramebufferStatus equivalent: an incomplete
        // attachment set is a validation error at record time, not a queryable
        // state. Reporting "Complete" would be inventing a check that never
        // ran, so this says what it actually knows.
        outClass = FramebufferStatusClass::Unknown;
        return "n/a (dynamic rendering — no framebuffer completeness query)";
    }

    // ---- Synchronous readback (unsupported — see the class comment) ---------

    namespace
    {
        constexpr const char* kNoIdReadback =
            "This backend has no native-id readback path. Pixel inspection goes through the facade "
            "readback spine (olo_render_capture_target / olo_render_probe_pixel / "
            "olo_render_target_stats), which resolves an RHI handle rather than a raw object name.";
    }

    bool VulkanResourceInspectorBackend::ReadTextureLevel(u64, bool, u32, u32, u32, u32, u32, bool, void*, sizet,
                                                          std::string& outError)
    {
        outError = kNoIdReadback;
        return false;
    }

    bool VulkanResourceInspectorBackend::ReadBufferRange(u64, u32, u32, u32, void*)
    {
        return false;
    }

    bool VulkanResourceInspectorBackend::QueryCaptureSource(u64, u32, CaptureSource& outSource)
    {
        outSource.Error = kNoIdReadback;
        return false;
    }

    bool VulkanResourceInspectorBackend::ReadCaptureRegion(u64, u32, u32, const CaptureSource&, u32, u32, u32, u32,
                                                           void*, sizet, std::string& outError)
    {
        outError = kNoIdReadback;
        return false;
    }

    bool VulkanResourceInspectorBackend::CaptureRowsAreBottomUp() const
    {
        // One row order per backend (ADR 0011 amendment (85)): every off-screen
        // target is top-down here.
        return false;
    }

    // ---- Async download engine (unsupported) --------------------------------

    bool VulkanResourceInspectorBackend::BeginTextureDownload(u64, bool, u32, u32, u32, u32, sizet,
                                                              DownloadTicket&)
    {
        return false;
    }

    IResourceInspectorBackend::DownloadStatus VulkanResourceInspectorBackend::PollDownload(const DownloadTicket&)
    {
        return DownloadStatus::Failed;
    }

    const void* VulkanResourceInspectorBackend::MapDownloadData(const DownloadTicket&, sizet)
    {
        return nullptr;
    }

    void VulkanResourceInspectorBackend::UnmapDownloadData(const DownloadTicket&)
    {
    }

    void VulkanResourceInspectorBackend::ReleaseDownload(const DownloadTicket&)
    {
    }

    // ---- ImGui binding ------------------------------------------------------

    u64 VulkanResourceInspectorBackend::GetImGuiTextureID(u64) const
    {
        // 0 = no binding, the same contract as ImGuiLayer::GetTextureID: an
        // ImTextureID here would have to be a VkDescriptorSet minted per image
        // by the ImGui Vulkan renderer backend, which the panel has no lifetime
        // story for. Callers skip the ImGui::Image draw.
        return 0u;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
