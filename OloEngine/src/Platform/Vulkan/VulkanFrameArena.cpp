#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanRecordingContext.h"
#include "Platform/Vulkan/VulkanTransientResources.h"
#include "OloEngine/Memory/AlignmentTemplates.h"

#include <atomic>
#include <cstring>

namespace OloEngine
{
    VulkanFrameArena& VulkanFrameArena::Get()
    {
        static auto* s_Instance = new VulkanFrameArena(); // deliberately leaked
        return *s_Instance;
    }

    void VulkanFrameArena::Slot::Reset()
    {
        Buffer = VK_NULL_HANDLE;
        Allocation = VK_NULL_HANDLE;
        Mapped = nullptr;
        BaseAddress = 0;
        Cursor.store(0, std::memory_order_relaxed);
        NeedsFlush = false;
    }

    bool VulkanFrameArena::EnsureBuffers()
    {
        if (m_Slots[0].Buffer != VK_NULL_HANDLE)
        {
            return true;
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            static bool s_Warned = false;
            if (!s_Warned)
            {
                s_Warned = true;
                OLO_CORE_WARN("VulkanFrameArena: no live VulkanDevice — root-data "
                              "allocations will fail until a device is up");
            }
            return false;
        }

        for (Slot& slot : m_Slots)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = kSlotCapacityBytes;
            // The one buffer serves every root-data consumer: UBO-shaped and
            // SSBO-shaped block mappings (INDIRECT_ADDRESS reads), GPU-written
            // indirect args (§4.2), and plain device-address dereferences.
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            // Persistently mapped, sequential-write: the CPU writes root
            // structs directly into GPU-visible memory. DEVICE_LOCAL is
            // preferred (ReBAR/UMA — the ADR 0010 driver floor makes this the
            // expected placement); VMA falls back to plain host-visible when
            // the BAR window can't take it, which is slower but correct.
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

            VmaAllocationInfo resultInfo{};
            const VkResult result = vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo,
                                                    &slot.Buffer, &slot.Allocation, &resultInfo);
            if (result != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanFrameArena: vmaCreateBuffer failed (VkResult {}) — releasing partial slots",
                               static_cast<int>(result));
                ReleaseBuffers();
                return false;
            }
            slot.Mapped = resultInfo.pMappedData;

            VkBufferDeviceAddressInfo addressInfo{};
            addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addressInfo.buffer = slot.Buffer;
            slot.BaseAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);
            slot.Cursor.store(0, std::memory_order_relaxed);

            // Real failure checks, not assert-only: a null mapping or a zero
            // device address in a release build would otherwise flow into
            // pointer arithmetic and root structs (asserts compile out).
            if (slot.Mapped == nullptr || slot.BaseAddress == 0)
            {
                OLO_CORE_ERROR("VulkanFrameArena: slot came up unusable (mapped={}, address={:#x}) — releasing",
                               slot.Mapped != nullptr, slot.BaseAddress);
                ReleaseBuffers();
                return false;
            }

            // AUTO + SEQUENTIAL_WRITE may land on non-coherent host-visible
            // memory (not on the desktop ReBAR floor, but correctness must not
            // depend on that): record whether writes need an explicit flush.
            VkMemoryPropertyFlags memoryFlags = 0;
            vmaGetAllocationMemoryProperties(device->GetAllocator(), slot.Allocation, &memoryFlags);
            slot.NeedsFlush = (memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
        }
        return true;
    }

    bool VulkanFrameArena::EnsureNullBlock()
    {
        if (m_NullBlockAddress != 0)
        {
            return true;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return false;
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kNullBlockBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        allocInfo.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        VmaAllocationInfo resultInfo{};
        if (vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &m_NullBlockBuffer,
                            &m_NullBlockAllocation, &resultInfo) != VK_SUCCESS ||
            resultInfo.pMappedData == nullptr)
        {
            OLO_CORE_ERROR("VulkanFrameArena: null-block creation failed — unfed bindings stay at address 0");
            if (m_NullBlockBuffer != VK_NULL_HANDLE || m_NullBlockAllocation != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(device->GetAllocator(), m_NullBlockBuffer, m_NullBlockAllocation);
                m_NullBlockBuffer = VK_NULL_HANDLE;
                m_NullBlockAllocation = VK_NULL_HANDLE;
            }
            return false;
        }
        std::memset(resultInfo.pMappedData, 0, kNullBlockBytes);
        vmaFlushAllocation(device->GetAllocator(), m_NullBlockAllocation, 0, kNullBlockBytes);

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_NullBlockBuffer;
        m_NullBlockAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);
        if (m_NullBlockAddress == 0)
        {
            // A zero address is the failure this block exists to prevent —
            // release the orphaned buffer so a later call can retry cleanly.
            OLO_CORE_ERROR("VulkanFrameArena: null-block device address is 0 — releasing and leaving unfed "
                           "bindings at address 0");
            vmaDestroyBuffer(device->GetAllocator(), m_NullBlockBuffer, m_NullBlockAllocation);
            m_NullBlockBuffer = VK_NULL_HANDLE;
            m_NullBlockAllocation = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void VulkanFrameArena::BeginFrame(u32 frameSlot)
    {
        OLO_CORE_ASSERT(frameSlot < kFramesInFlight, "VulkanFrameArena: frame slot out of range");
        OLO_CORE_ASSERT(CurrentVulkanWorkerContext() == nullptr,
                        "VulkanFrameArena: BeginFrame from a worker context — render thread only");
        // Creation happens HERE, not on the first draw: a parallel region's
        // workers allocate concurrently, and creating the buffers under one
        // of them would race every other (#806). Both helpers are no-ops
        // once created and log their own failures; a later Allocate /
        // GetNullBlockAddress then degrades exactly as before (a null
        // allocation / address 0).
        (void)EnsureBuffers();
        (void)EnsureNullBlock();
        m_CurrentSlot = frameSlot % kFramesInFlight;
        m_Slots[m_CurrentSlot].Cursor.store(0, std::memory_order_relaxed);
        ++m_FrameGeneration;
        m_AllocationsThisFrame.store(0, std::memory_order_relaxed);
    }

    VulkanFrameArenaAllocation VulkanFrameArena::Allocate(u64 sizeBytes, u64 alignment)
    {
        // Runtime checks, not assert-only: alignment == 0 wraps the mask and
        // aligned becomes 0, silently overwriting root data already handed
        // out this frame. Overflow test by subtraction so a huge sizeBytes
        // cannot wrap aligned + sizeBytes past the capacity.
        OLO_CORE_ASSERT(alignment != 0 && IsPowerOfTwo(alignment), "VulkanFrameArena: alignment must be a power of two");
        if (alignment == 0 || !IsPowerOfTwo(alignment))
        {
            return {};
        }
        if (m_Slots[0].Buffer == VK_NULL_HANDLE)
        {
            // Lazy fallback for callers that never BeginFrame (tests). Never
            // legal on a worker: creation is render-thread-only (header), and
            // BeginFrame has already created the buffers before any region
            // can fork — a worker landing here means a draw ran before the
            // frame began.
            OLO_CORE_ASSERT(CurrentVulkanWorkerContext() == nullptr,
                            "VulkanFrameArena: lazy buffer creation from a worker context — BeginFrame must run first");
            if (!EnsureBuffers())
            {
                return {};
            }
        }
        if (sizeBytes == 0)
        {
            return {};
        }

        Slot& slot = m_Slots[m_CurrentSlot];

        // The shared claim (#806): one compare-exchange over the slot's
        // cursor. Two threads that read the same cursor compute the same
        // [aligned, end) range, but only one CAS succeeds; the loser gets the
        // cursor the winner advanced to and retries past the winner's range,
        // so claimed ranges are disjoint by construction. Relaxed ordering
        // is enough: a claimed range is written only by the thread that
        // claimed it, and the GPU's read of it is ordered by the submit that
        // follows the region's join, not by this cursor.
        const auto claimShared = [&](const u64 bytes, const u64 align, u64& outOffset) -> bool
        {
            u64 cursor = slot.Cursor.load(std::memory_order_relaxed);
            for (;;)
            {
                const u64 aligned = Align(cursor, align);
                if (aligned > kSlotCapacityBytes || bytes > kSlotCapacityBytes - aligned)
                {
                    return false;
                }
                if (slot.Cursor.compare_exchange_weak(cursor, aligned + bytes, std::memory_order_relaxed))
                {
                    outOffset = aligned;
                    return true;
                }
                // A failed CAS reloaded `cursor` with the value another
                // thread advanced it to; recompute against that.
            }
        };
        const auto overflow = [&](const u64 at)
        {
            m_OverflowCount.fetch_add(1, std::memory_order_relaxed);
            if (!m_OverflowWarned.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_ERROR("VulkanFrameArena: slot {} overflow ({} B requested at cursor {} of {} B) — "
                               "root-data allocations are being DROPPED this frame",
                               m_CurrentSlot, sizeBytes, at, kSlotCapacityBytes);
            }
            return VulkanFrameArenaAllocation{};
        };

        // A RecordParallel item bumps inside its own block and refills it from
        // the shared cursor one block at a time, so the per-draw push touches
        // no shared cache line (the measured hot spot: every draw on every
        // worker claimed through one CAS).
        if (auto* worker = CurrentVulkanWorkerContext(); worker != nullptr && sizeBytes <= kWorkerBlockBytes / 2u)
        {
            auto& block = worker->Arena;
            u64 aligned = Align(block.Cursor, alignment);
            if (block.End == 0u || aligned + sizeBytes > block.End)
            {
                u64 blockOffset = 0;
                if (!claimShared(kWorkerBlockBytes, std::max<u64>(alignment, 256u), blockOffset))
                {
                    return overflow(blockOffset);
                }
                block.Cursor = blockOffset;
                block.End = blockOffset + kWorkerBlockBytes;
                aligned = Align(block.Cursor, alignment);
            }
            block.Cursor = aligned + sizeBytes;
            ++worker->ArenaAllocations;
            return {
                .Cpu = static_cast<u8*>(slot.Mapped) + aligned,
                .Gpu = slot.BaseAddress + aligned,
                .Offset = aligned,
            };
        }

        u64 aligned = 0;
        if (!claimShared(sizeBytes, alignment, aligned))
        {
            return overflow(slot.Cursor.load(std::memory_order_relaxed));
        }
        m_AllocationsThisFrame.fetch_add(1, std::memory_order_relaxed);
        return {
            .Cpu = static_cast<u8*>(slot.Mapped) + aligned,
            .Gpu = slot.BaseAddress + aligned,
            .Offset = aligned,
        };
    }

    void VulkanFrameArena::AddWorkerAllocations(const u64 count)
    {
        m_AllocationsThisFrame.fetch_add(count, std::memory_order_relaxed);
    }

    VulkanFrameArenaAllocation VulkanFrameArena::Push(const void* data, u64 sizeBytes, u64 alignment)
    {
        VulkanFrameArenaAllocation allocation = Allocate(sizeBytes, alignment);
        if (allocation.IsValid() && data != nullptr)
        {
            std::memcpy(allocation.Cpu, data, sizeBytes);
            FlushWrite(allocation, sizeBytes);
        }
        return allocation;
    }

    void VulkanFrameArena::FlushWrite(const VulkanFrameArenaAllocation& allocation, u64 sizeBytes)
    {
        // No-op on coherent memory (the desktop ReBAR expectation). On a
        // non-coherent placement, host writes are invisible to the GPU until
        // flushed — Push() calls this itself; direct writers through
        // Allocate() own the call. Safe to call concurrently: every caller
        // flushes only the range it claimed, and the slot fields read here
        // are creation-time constants.
        const Slot& slot = m_Slots[m_CurrentSlot];
        if (!slot.NeedsFlush || !allocation.IsValid())
        {
            return;
        }
        auto* device = VulkanDevice::Get();
        if (device != nullptr)
        {
            vmaFlushAllocation(device->GetAllocator(), slot.Allocation, allocation.Offset, sizeBytes);
        }
    }

    VkDeviceAddress VulkanFrameArena::GetNullBlockAddress()
    {
        if (m_NullBlockAddress != 0)
        {
            return m_NullBlockAddress;
        }
        // Lazy fallback for callers that never BeginFrame (tests). BeginFrame
        // creates the block before any region can fork, so a worker reaching
        // this line means a draw ran before the frame began.
        OLO_CORE_ASSERT(CurrentVulkanWorkerContext() == nullptr,
                        "VulkanFrameArena: lazy null-block creation from a worker context — BeginFrame must run first");
        (void)EnsureNullBlock();
        return m_NullBlockAddress;
    }

    void VulkanFrameArena::ReleaseBuffers()
    {
        for (Slot& slot : m_Slots)
        {
            if (slot.Buffer != VK_NULL_HANDLE || slot.Allocation != VK_NULL_HANDLE)
            {
                VulkanDeferredReclaim::Get().Enqueue(slot.Buffer, slot.Allocation);
            }
            slot.Reset();
        }
        if (m_NullBlockBuffer != VK_NULL_HANDLE || m_NullBlockAllocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_NullBlockBuffer, m_NullBlockAllocation);
            m_NullBlockBuffer = VK_NULL_HANDLE;
            m_NullBlockAllocation = VK_NULL_HANDLE;
            m_NullBlockAddress = 0;
        }
        m_CurrentSlot = 0;
        m_AllocationsThisFrame.store(0, std::memory_order_relaxed);
    }

    u64 VulkanFrameArena::GetCurrentSlotUsedBytes() const
    {
        return m_Slots[m_CurrentSlot].Cursor.load(std::memory_order_relaxed);
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
