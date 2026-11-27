#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTextureCubemapArray.h"
#include "Platform/Vulkan/VulkanTrackedAllocation.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanImageInfoRegistry.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kFacesPerCube = 6u;

        u32 ResidentBytesPerTexel(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                case ImageFormat::RGBA8:
                case ImageFormat::R32F:
                case ImageFormat::RG32F:
                case ImageFormat::RGBA16F:
                case ImageFormat::RGBA32F:
                    return VulkanUpload::EngineFormatClientBpp(format);
                default:
                    return 0u;
            }
        }

        bool RegisterImage(VulkanImageLayoutTracker& tracker, VkImage image)
        {
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
            if (info == nullptr)
                return false;
            tracker.RegisterImage(image, info->MipLevels, info->ArrayLayers, info->RegistrationId, info->InitialLayout);
            return true;
        }

        void Transition(VkCommandBuffer cmd, VulkanImageLayoutTracker& tracker, VkImage image,
                        const VkImageSubresourceRange& range, VkImageLayout target,
                        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
        {
            TArray<VkImageMemoryBarrier2> barriers;
            tracker.ForEachLayoutRun(image, range,
                                     [&](const VkImageSubresourceRange& run, VkImageLayout prior)
                                     {
                                         if (prior == target && dstStage != VK_PIPELINE_STAGE_2_COPY_BIT)
                                             return;
                                         VkImageMemoryBarrier2 barrier{};
                                         barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                                         if (prior != VK_IMAGE_LAYOUT_UNDEFINED)
                                         {
                                             barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                                             barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                                         }
                                         barrier.dstStageMask = dstStage;
                                         barrier.dstAccessMask = dstAccess;
                                         barrier.oldLayout = prior;
                                         barrier.newLayout = target;
                                         barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                         barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                         barrier.image = image;
                                         barrier.subresourceRange = run;
                                         barriers.Add(barrier);
                                     });
            if (barriers.IsEmpty())
                return;

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = static_cast<u32>(barriers.Num());
            dep.pImageMemoryBarriers = barriers.GetData();
            const auto* api = VulkanUpload::TryGetRecordingVulkanAPI();
            if (api != nullptr && api->CurrentCommandBuffer() == cmd)
                api->CurrentRecordingContext().RecordBarrier(dep);
            else
                vkCmdPipelineBarrier2(cmd, &dep);
            tracker.SetLayout(image, range, target);
        }

        struct LayoutRun
        {
            VkImageSubresourceRange Range;
            VkImageLayout Layout;
        };

        bool CaptureReadableLayouts(VulkanImageLayoutTracker& tracker, VkImage image,
                                    const VkImageSubresourceRange& range, TArray<LayoutRun>& layouts)
        {
            if (!RegisterImage(tracker, image))
                return false;

            bool readable = true;
            tracker.ForEachLayoutRun(image, range,
                                     [&](const VkImageSubresourceRange& run, VkImageLayout layout)
                                     {
                                         readable &= layout != VK_IMAGE_LAYOUT_UNDEFINED;
                                         layouts.Add(LayoutRun{ run, layout });
                                     });
            return readable && !layouts.IsEmpty();
        }

        void RestoreLayouts(VkCommandBuffer cmd, VulkanImageLayoutTracker& tracker, VkImage image,
                            const TArray<LayoutRun>& layouts)
        {
            for (const auto& run : layouts)
                Transition(cmd, tracker, image, run.Range, run.Layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        }
    } // namespace

    VulkanTextureCubemapArray::VulkanTextureCubemapArray(const CubemapArraySpecification& spec)
        : m_ArraySpecification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTextureCubemapArray requires a live VulkanDevice");

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device->GetPhysicalDevice(), &properties);
        const u32 resolution = spec.Resolution;
        const u32 layers = spec.Layers;
        const u32 fullCubeArrayChain = std::bit_width(resolution);
        if (resolution == 0u || resolution > properties.limits.maxImageDimensionCube || layers == 0u ||
            layers > properties.limits.maxImageArrayLayers / kFacesPerCube || spec.MipLevels > fullCubeArrayChain ||
            ResidentBytesPerTexel(spec.Format) == 0u)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray: unsupported specification ({}x{}, {} layers, {} mips, format {})",
                           resolution, resolution, layers, spec.MipLevels, static_cast<u32>(spec.Format));
            return;
        }
        m_Specification.Width = resolution;
        m_Specification.Height = resolution;
        m_Specification.Format = spec.Format;
        m_Specification.GenerateMips = spec.MipLevels != 1u;
        m_Specification.MipLevels = spec.MipLevels;
        m_MipLevels = spec.MipLevels > 0u ? spec.MipLevels : fullCubeArrayChain;
        m_Path = "Generated CubemapArray";

        const VkFormat format = VulkanUpload::ImageFormatToVkFormat(spec.Format, false);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        // CUBE_COMPATIBLE + 6*N layers is what makes the image addressable as
        // samplerCubeArray (the view type carries the rest).
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { resolution, resolution, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = kFacesPerCube * layers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (TrackedVmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray: image creation failed ({}x{}, {} layers, {} mips)", resolution,
                           resolution, layers, m_MipLevels);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }
        TrackedVmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanTextureCubemapArray");

        VulkanImageInfo registryInfo{};
        registryInfo.Format = format;
        registryInfo.Width = resolution;
        registryInfo.Height = resolution;
        registryInfo.MipLevels = m_MipLevels;
        registryInfo.ArrayLayers = imageInfo.arrayLayers;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        // §4f sampler table: cubemaps are CLAMP_TO_EDGE on GL — the array
        // flavour follows its element type.
        registryInfo.AddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTextureCubemapArray::~VulkanTextureCubemapArray()
    {
        try
        {
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
            {
                // Slots free in DestroyEntry, not here — the
                // VulkanDescriptorSlotCache recycling contract.
                VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
                m_Image = VK_NULL_HANDLE;
                m_Allocation = VK_NULL_HANDLE;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray dtor: reclaim enqueue threw ({}); the array leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray dtor: reclaim enqueue threw; the array leaks");
        }
    }

    void VulkanTextureCubemapArray::Bind(u32 slot) const
    {
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTextureCubemapArray::SetData(void* /*data*/, u32 /*size*/)
    {
        OLO_CORE_ERROR("SetData is not supported for cubemap arrays; use SetLayerMipData");
    }

    void VulkanTextureCubemapArray::Invalidate(std::string_view /*path*/, u32 /*width*/, u32 /*height*/,
                                               const void* /*data*/, u32 /*channels*/)
    {
        OLO_CORE_ERROR("Invalidate is not supported for cubemap arrays");
    }

    bool VulkanTextureCubemapArray::Write(const char* label, const Transfer& record, VkBuffer staging,
                                          VmaAllocation stagingAllocation) const
    {
        auto* api = VulkanUpload::TryGetVulkanAPI();
        const auto allocator = VulkanDevice::Get()->GetAllocator();
        if (api != nullptr && api->CurrentCommandBuffer() != VK_NULL_HANDLE)
        {
            if (api->RefuseOnWorker(label) || api->IsInParallelRegion())
            {
                if (staging != VK_NULL_HANDLE)
                    TrackedVmaDestroyBuffer(allocator, staging, stagingAllocation);
                return false;
            }
            api->EndRenderingScope();
            const bool recorded = record(api->CurrentCommandBuffer(), api->LayoutTracker());
            if (staging != VK_NULL_HANDLE)
            {
                if (recorded)
                    VulkanDeferredReclaim::Get().Enqueue(staging, stagingAllocation);
                else
                    TrackedVmaDestroyBuffer(allocator, staging, stagingAllocation);
            }
            return recorded;
        }

        // A bare-device fixture has no Vulkan facade. The array's one-shot
        // writes finish in the uniform layout recorded by the image registry.
        VulkanImageLayoutTracker standalone;
        auto& tracker = api != nullptr ? api->LayoutTracker() : standalone;
        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, m_ArraySpecification.Layers * kFacesPerCube };
        TArray<LayoutRun> prior;
        VulkanOneShot::Outcome outcome{};
        bool recorded = false;
        const bool ok = VulkanOneShot::Submit(label, [&](VkCommandBuffer cmd)
                                              {
                                                  if (!RegisterImage(tracker, m_Image))
                                                      return;
                                                  tracker.ForEachLayoutRun(m_Image, whole, [&](const VkImageSubresourceRange& range, VkImageLayout layout)
                                                                           { prior.Add(LayoutRun{ range, layout }); });
                                                  recorded = record(cmd, tracker); }, &outcome);
        if (staging != VK_NULL_HANDLE && outcome != VulkanOneShot::Outcome::DrainFailed)
            TrackedVmaDestroyBuffer(allocator, staging, stagingAllocation);
        if (recorded && outcome != VulkanOneShot::Outcome::NotSubmitted)
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VulkanDevice::Get()->GetSampledImageLayout());
        else if (outcome == VulkanOneShot::Outcome::NotSubmitted)
        {
            for (const auto& run : prior)
                tracker.SetLayout(m_Image, run.Range, run.Layout);
        }
        return ok && recorded;
    }

    bool VulkanTextureCubemapArray::SetLayerMipData(u32 layer, u32 mip, const void* data, sizet sizeBytes)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        if (device == nullptr || !IsLoaded() || layer >= m_ArraySpecification.Layers || mip >= m_MipLevels || data == nullptr)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray::SetLayerMipData: invalid destination or data (layer {}, mip {})", layer, mip);
            return false;
        }

        const u32 extent = std::max(1u, m_ArraySpecification.Resolution >> mip);
        const u64 texels = static_cast<u64>(extent) * extent * kFacesPerCube;
        const u64 storedSize = texels * ResidentBytesPerTexel(m_ArraySpecification.Format);
        const bool packHalf = m_ArraySpecification.Format == ImageFormat::RGBA16F;
        const u64 expected = packHalf ? texels * sizeof(f32) * 4u : storedSize;
        if (sizeBytes != expected)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray::SetLayerMipData: expected {} bytes, got {}", expected, sizeBytes);
            return false;
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = storedSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo mapped{};
        const auto allocator = device->GetAllocator();
        if (TrackedVmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &staging, &allocation, &mapped) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray::SetLayerMipData: staging allocation failed ({} bytes)", storedSize);
            return false;
        }
        if (packHalf)
        {
            // Like GL, the array accepts full float components for RGBA16F.
            const auto* src = static_cast<const u8*>(data);
            auto* dst = static_cast<u32*>(mapped.pMappedData);
            for (u64 pair = 0; pair < texels * 2u; ++pair)
            {
                std::array<f32, 2> values{};
                std::memcpy(values.data(), src + pair * sizeof(values), sizeof(values));
                dst[pair] = glm::packHalf2x16({ values[0], values[1] });
            }
        }
        else
        {
            std::memcpy(mapped.pMappedData, data, storedSize);
        }
        if (vmaFlushAllocation(allocator, allocation, 0, storedSize) != VK_SUCCESS)
        {
            TrackedVmaDestroyBuffer(allocator, staging, allocation);
            OLO_CORE_ERROR("VulkanTextureCubemapArray::SetLayerMipData: staging flush failed");
            return false;
        }

        const u32 baseFace = layer * kFacesPerCube;
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, mip, 1u, baseFace, kFacesPerCube };
        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, m_ArraySpecification.Layers * kFacesPerCube };
        return Write("VulkanTextureCubemapArray::SetLayerMipData", [&](VkCommandBuffer cmd, VulkanImageLayoutTracker& tracker)
                     {
                         if (!RegisterImage(tracker, m_Image))
                             return false;
                         Transition(cmd, tracker, m_Image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                         VkBufferImageCopy copy{};
                         copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, baseFace, kFacesPerCube };
                         copy.imageExtent = { extent, extent, 1u };
                         vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
                         // The sampled descriptor covers the whole array, including unused slots.
                         Transition(cmd, tracker, m_Image, whole, device->GetSampledImageLayout(),
                                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
                         return true; }, staging, allocation);
    }

    bool VulkanTextureCubemapArray::CopyLayerFromCubemap(u32 layer, const TextureCubemap& source)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        const auto& spec = source.GetCubemapSpecification();
        if (device == nullptr || !IsLoaded() || layer >= m_ArraySpecification.Layers || !source.IsLoaded() ||
            spec.Width != m_ArraySpecification.Resolution || spec.Height != spec.Width || spec.Format != m_ArraySpecification.Format)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray::CopyLayerFromCubemap: invalid source or destination (layer {})", layer);
            return false;
        }
        const auto native = RHI::ResourceRegistry::Get().ResolveTaggedForBackend(source.GetRHIHandle());
        const auto sourceImage = native.Owner == RHI::Backend::Vulkan ? reinterpret_cast<VkImage>(native.Value) : VK_NULL_HANDLE;
        const auto* sourceInfo = VulkanImageInfoRegistry::Get().Lookup(sourceImage);
        const auto* arrayInfo = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        if (sourceInfo == nullptr || arrayInfo == nullptr || sourceInfo->ViewType != VK_IMAGE_VIEW_TYPE_CUBE ||
            sourceInfo->Format != arrayInfo->Format)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray::CopyLayerFromCubemap: source has no compatible Vulkan cubemap image");
            return false;
        }

        const u32 mipCount = std::min(m_MipLevels, source.GetMipLevelCount());
        const u32 baseFace = layer * kFacesPerCube;
        const VkImageSubresourceRange sourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, mipCount, 0u, kFacesPerCube };
        const VkImageSubresourceRange destRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, mipCount, baseFace, kFacesPerCube };
        const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, m_ArraySpecification.Layers * kFacesPerCube };
        return Write("VulkanTextureCubemapArray::CopyLayerFromCubemap", [&](VkCommandBuffer cmd, VulkanImageLayoutTracker& tracker)
                     {
                         TArray<LayoutRun> previous;
                         if (!CaptureReadableLayouts(tracker, sourceImage, sourceRange, previous) || !RegisterImage(tracker, m_Image))
                         {
                             OLO_CORE_ERROR("VulkanTextureCubemapArray::CopyLayerFromCubemap: source subresources have no readable layout");
                             return false;
                         }
                         std::array<VkImageCopy, 32> copies{};
                         for (u32 mip = 0; mip < mipCount; ++mip)
                         {
                             const u32 extent = std::max(1u, spec.Width >> mip);
                             auto& copy = copies[mip];
                             copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0u, kFacesPerCube };
                             copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, baseFace, kFacesPerCube };
                             copy.extent = { extent, extent, 1u };
                         }
                         Transition(cmd, tracker, sourceImage, sourceRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                         Transition(cmd, tracker, m_Image, destRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                         vkCmdCopyImage(cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipCount, copies.data());
                         RestoreLayouts(cmd, tracker, sourceImage, previous);
                         Transition(cmd, tracker, m_Image, whole, device->GetSampledImageLayout(),
                                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
                         return true; });
    }

    bool VulkanTextureCubemapArray::GetData(TArray64<u8>& outData, u32 mipLevel) const
    {
        OLO_PROFILE_FUNCTION();
        outData.Reset();
        auto* device = VulkanDevice::Get();
        if (device == nullptr || !IsLoaded() || mipLevel >= m_MipLevels)
            return false;
        auto* api = VulkanUpload::TryGetVulkanAPI();
        if (api != nullptr && api->CurrentCommandBuffer() != VK_NULL_HANDLE)
        {
            auto* context = VulkanContext::Get();
            if (api->RefuseOnWorker("VulkanTextureCubemapArray::GetData") || context == nullptr || !context->FlushFrameRecordingAndWait())
            {
                OLO_CORE_ERROR("VulkanTextureCubemapArray::GetData: cannot submit pending array writes");
                return false;
            }
        }

        VulkanImageLayoutTracker standalone;
        auto& tracker = api != nullptr ? api->LayoutTracker() : standalone;
        const u32 extent = std::max(1u, m_ArraySpecification.Resolution >> mipLevel);
        const u32 faces = kFacesPerCube * m_ArraySpecification.Layers;
        const u64 texels = static_cast<u64>(extent) * extent * faces;
        const u64 storedSize = texels * ResidentBytesPerTexel(m_ArraySpecification.Format);
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1u, 0u, faces };
        TArray<LayoutRun> previous;
        if (!CaptureReadableLayouts(tracker, m_Image, range, previous))
            return false;

        const bool unpackHalf = m_ArraySpecification.Format == ImageFormat::RGBA16F;
        // Allocate the caller's output before we acquire a Vulkan buffer.
        outData.SetNum(unpackHalf ? texels * sizeof(f32) * 4u : storedSize, EAllowShrinking::No);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = storedSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo mapped{};
        const auto allocator = device->GetAllocator();
        if (TrackedVmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &readback, &allocation, &mapped) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray::GetData: readback allocation failed ({} bytes)", storedSize);
            outData.Reset();
            return false;
        }

        VulkanOneShot::Outcome outcome{};
        const bool completed = VulkanOneShot::Submit("VulkanTextureCubemapArray::GetData", [&](VkCommandBuffer cmd)
                                                     {
                                                         Transition(cmd, tracker, m_Image, range, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                                                         VkBufferImageCopy copy{};
                                                         copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0u, faces };
                                                         copy.imageExtent = { extent, extent, 1u };
                                                         vkCmdCopyImageToBuffer(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &copy);
                                                         VkBufferMemoryBarrier2 hostRead{};
                                                         hostRead.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                                                         hostRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                                                         hostRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                                                         hostRead.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                                                         hostRead.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                                                         hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                                         hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                                         hostRead.buffer = readback;
                                                         hostRead.size = VK_WHOLE_SIZE;
                                                         VkDependencyInfo dep{};
                                                         dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                                                         dep.bufferMemoryBarrierCount = 1u;
                                                         dep.pBufferMemoryBarriers = &hostRead;
                                                         vkCmdPipelineBarrier2(cmd, &dep);
                                                         RestoreLayouts(cmd, tracker, m_Image, previous); }, &outcome);

        const bool readable = completed && vmaInvalidateAllocation(allocator, allocation, 0, storedSize) == VK_SUCCESS;
        if (!readable)
        {
            outData.Reset();
            if (completed)
                OLO_CORE_ERROR("VulkanTextureCubemapArray::GetData: readback invalidation failed");
        }
        else if (unpackHalf)
        {
            const auto* bytes = static_cast<const u32*>(mapped.pMappedData);
            for (u64 pair = 0; pair < texels * 2u; ++pair)
            {
                const auto rg = glm::unpackHalf2x16(bytes[pair]);
                const std::array values{ rg.x, rg.y };
                std::memcpy(outData.GetData() + pair * sizeof(values), values.data(), sizeof(values));
            }
        }
        else
        {
            std::memcpy(outData.GetData(), mapped.pMappedData, storedSize);
        }
        if (outcome != VulkanOneShot::Outcome::DrainFailed)
            TrackedVmaDestroyBuffer(allocator, readback, allocation);
        return readable;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
