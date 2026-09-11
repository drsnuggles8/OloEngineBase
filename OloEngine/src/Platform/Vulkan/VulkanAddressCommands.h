#pragma once

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include <volk.h>

namespace OloEngine::VulkanAddressCommands
{
    // -------------------------------------------------------------------------
    // VK_KHR_device_address_commands (#1179) — the shared vocabulary.
    //
    // The extension replaces the VkBuffer handle in a command's signature with
    // a {VkDeviceAddress, VkDeviceSize} range. It does NOT replace the buffer's
    // create-time usage declaration: every address form carries forward the
    // same usage VUID its handle form had, so the bits stay where they are.
    //
    //   VUID-VkBindIndexBuffer3InfoKHR-addressRange-13051  -> INDEX_BUFFER_BIT
    //   VUID-VkDeviceMemoryCopyKHR-srcRange-13017          -> TRANSFER_SRC_BIT
    //   VUID-VkDeviceMemoryCopyKHR-dstRange-13018          -> TRANSFER_DST_BIT
    //   VUID-VkDrawIndirect2InfoKHR-addressRange-13107     -> INDIRECT_BUFFER_BIT
    //   VUID-VkDispatchIndirect2InfoKHR-addressRange-13107 -> INDIRECT_BUFFER_BIT
    //
    // What the address form DOES need that the handle form did not is
    // SHADER_DEVICE_ADDRESS_BIT — without it vkGetBufferDeviceAddress cannot
    // return a range to pass at all. Staging and readback buffers were created
    // with a single transfer bit and had to gain it.
    // -------------------------------------------------------------------------

    // The STORAGE_BUFFER half of `addressFlags` is a two-directional
    // obligation, not a hint. For the memory backing the range:
    //
    //   VUID-*-13122: if ANY buffer bound to an overlapping range of
    //                 VkDeviceMemory was created WITH STORAGE_BUFFER_BIT, the
    //                 flags MUST include STORAGE_BUFFER_USAGE or
    //                 UNKNOWN_STORAGE_BUFFER_USAGE.
    //   VUID-*-13123: if it was created WITHOUT STORAGE_BUFFER_BIT, the flags
    //                 MUST NOT include STORAGE_BUFFER_USAGE.
    //
    // So a blanket 0 is wrong for storage-capable buffers and a blanket
    // STORAGE_BUFFER_USAGE is wrong for the rest — the caller has to state
    // which it holds. Both engine index-buffer kinds prove the point:
    // VulkanIndexBuffer is created INDEX|TRANSFER|ADDRESS (no storage bit),
    // while the raw arena behind SetVertexArrayIndexBuffer is a dual-role
    // element/SSBO buffer that does carry it.
    //
    // VMA suballocates many buffers out of one VkDeviceMemory block, but
    // suballocations do not overlap each other, so "any buffer overlapping
    // this range" is this buffer — its own create-time usage is the answer.
    enum class StorageUsage
    {
        // The buffer was created WITHOUT VK_BUFFER_USAGE_STORAGE_BUFFER_BIT.
        Absent,
        // The buffer was created WITH VK_BUFFER_USAGE_STORAGE_BUFFER_BIT.
        Present,
        // The site cannot know — an opaque handle whose creator is not fixed.
        // Legal under both 13122 and 13123, at the cost of telling the driver
        // less than we could. Prefer a definite answer wherever the creating
        // code is reachable; this is a truthful "unknown", not a default.
        Unknown,
    };

    [[nodiscard]] inline VkAddressCommandFlagsKHR FlagsFor(StorageUsage storage)
    {
        switch (storage)
        {
            case StorageUsage::Present:
                return VK_ADDRESS_COMMAND_STORAGE_BUFFER_USAGE_BIT_KHR;
            case StorageUsage::Unknown:
                return VK_ADDRESS_COMMAND_UNKNOWN_STORAGE_BUFFER_USAGE_BIT_KHR;
            case StorageUsage::Absent:
                break;
        }
        return 0;
    }

    // Deliberately NOT set anywhere in this backend:
    //
    //  - VK_ADDRESS_COMMAND_PROTECTED_BIT_KHR: no buffer here is created with
    //    VK_BUFFER_CREATE_PROTECTED_BIT, and VUID-*-13099 forbids the flag on
    //    unprotected memory when protectedNoFault is unsupported.
    //  - VK_ADDRESS_COMMAND_TRANSFORM_FEEDBACK_BUFFER_USAGE_BIT_KHR: the
    //    backend creates no transform-feedback buffers, and VUID-*-13125
    //    forbids the flag when the bit is absent. (Same two-directional shape
    //    as the storage pair above — it just has one legal answer here.)
    //  - VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR: a pure optimisation hint,
    //    and VUID-*-13097 makes it a violation if any part of the range is
    //    unbound. Every range here is VMA-backed and fully resident, so the
    //    bit would be legal — it stays off because nothing measures a win from
    //    it yet, and a wrong "fully bound" claim is a device-lost class of bug.

    [[nodiscard]] inline VkDeviceAddressRangeKHR MakeRange(VkDeviceAddress address, VkDeviceSize size)
    {
        VkDeviceAddressRangeKHR range{};
        range.address = address;
        range.size = size;
        return range;
    }

    [[nodiscard]] inline VkStridedDeviceAddressRangeKHR MakeStridedRange(VkDeviceAddress address, VkDeviceSize size,
                                                                        VkDeviceSize stride)
    {
        VkStridedDeviceAddressRangeKHR range{};
        range.address = address;
        range.size = size;
        range.stride = stride;
        return range;
    }

    // Create-time usage for a transient staging buffer feeding an address-form
    // copy. SHADER_DEVICE_ADDRESS is the bit the conversion ADDS: without it
    // vkGetBufferDeviceAddress cannot return a range for the buffer at all, so
    // there is nothing to pass vkCmdCopyMemoryKHR / vkCmdCopyMemoryToImageKHR.
    // TRANSFER_SRC is unchanged — VUID-VkDeviceMemoryCopyKHR-srcRange-13017
    // demands it of the address form exactly as the handle form did.
    //
    // No STORAGE_BUFFER_BIT, so every staging range pairs with
    // StorageUsage::Absent (VUID-13123 forbids the flag).
    inline constexpr VkBufferUsageFlags kStagingSrcUsage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    // The readback twin: a DeviceToHost buffer that is only ever a copy
    // destination (VUID-VkDeviceMemoryCopyKHR-dstRange-13018).
    inline constexpr VkBufferUsageFlags kReadbackDstUsage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    [[nodiscard]] inline VkDeviceAddress QueryAddress(VkDevice device, VkBuffer buffer)
    {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer;
        return vkGetBufferDeviceAddress(device, &info);
    }

    // vkCmdCopyMemoryKHR for one contiguous region — the shape every
    // vkCmdCopyBuffer in this backend had (all of them pass regionCount 1).
    inline void CmdCopyRange(VkCommandBuffer cmd, VkDeviceAddress srcAddress, StorageUsage srcStorage,
                             VkDeviceAddress dstAddress, StorageUsage dstStorage, VkDeviceSize sizeBytes)
    {
        VkDeviceMemoryCopyKHR region{};
        region.sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR;
        region.srcRange = MakeRange(srcAddress, sizeBytes);
        region.srcFlags = FlagsFor(srcStorage);
        region.dstRange = MakeRange(dstAddress, sizeBytes);
        region.dstFlags = FlagsFor(dstStorage);

        VkCopyDeviceMemoryInfoKHR copyInfo{};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR;
        copyInfo.regionCount = 1u;
        copyInfo.pRegions = &region;
        vkCmdCopyMemoryKHR(cmd, &copyInfo);
    }

    // vkCmdCopyMemoryToImageKHR for one region. The struct's
    // `addressRowLength` / `addressImageHeight` are VkBufferImageCopy's
    // `bufferRowLength` / `bufferImageHeight` under new names; both stay 0
    // here, the tightly-packed meaning every call site in this backend used.
    inline void CmdCopyRangeToImage(VkCommandBuffer cmd, VkDeviceAddress srcAddress, StorageUsage srcStorage,
                                    VkDeviceSize sizeBytes, VkImage image, VkImageLayout layout,
                                    const VkImageSubresourceLayers& subresource, VkOffset3D imageOffset,
                                    VkExtent3D imageExtent)
    {
        VkDeviceMemoryImageCopyKHR region{};
        region.sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR;
        region.addressRange = MakeRange(srcAddress, sizeBytes);
        region.addressFlags = FlagsFor(srcStorage);
        region.imageSubresource = subresource;
        region.imageLayout = layout;
        region.imageOffset = imageOffset;
        region.imageExtent = imageExtent;

        VkCopyDeviceMemoryImageInfoKHR copyInfo{};
        copyInfo.sType = VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR;
        copyInfo.image = image;
        copyInfo.regionCount = 1u;
        copyInfo.pRegions = &region;
        vkCmdCopyMemoryToImageKHR(cmd, &copyInfo);
    }
} // namespace OloEngine::VulkanAddressCommands

#endif // OLO_WITH_VULKAN
