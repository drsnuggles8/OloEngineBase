#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanStorageBuffer.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDeferredReclaim.h"
#include "Platform/Vulkan/VulkanFrameArena.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"
#include "Platform/Vulkan/VulkanTransientUpload.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace OloEngine
{
    VulkanStorageBuffer::VulkanStorageBuffer(u32 size, u32 binding, StorageBufferUsage usage)
        : m_Size(size), m_Binding(binding), m_Usage(usage)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanStorageBuffer requires a live VulkanDevice");

        CreateBuffer();
        VulkanUpload::TrackLive(this, "VulkanStorageBuffer");
    }

    VulkanStorageBuffer::~VulkanStorageBuffer()
    {
        VulkanUpload::UntrackLive(this);
        // Identity first, then the deferred queue — never vmaDestroyBuffer
        // inline (prior frames may still be executing). No exception may
        // escape a destructor: a failed enqueue leaks one buffer until
        // process exit, which beats std::terminate.
        try
        {
            VulkanBindingState::Get().ClearBuffer(this);
            VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            ReleaseBuffer();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanStorageBuffer: release failed ({}) — leaking the buffer until process exit", e.what());
        }
    }

    void VulkanStorageBuffer::CreateBuffer()
    {
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanStorageBuffer::CreateBuffer requires a live VulkanDevice");
        if (device == nullptr)
        {
            throw std::runtime_error("VulkanStorageBuffer::CreateBuffer: no live VulkanDevice");
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        // Vulkan refuses a zero-sized buffer; clamp defensively (GetSize keeps
        // reporting the authored size).
        bufferInfo.size = std::max<VkDeviceSize>(m_Size, 1u);
        // SHADER_DEVICE_ADDRESS: Phase 6's root-data model (ADR 0011 §4)
        // addresses buffers by VkDeviceAddress embedded in the root struct, so
        // every storage buffer must be addressable. bufferDeviceAddress is
        // enabled at device creation and the VMA allocator carries
        // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT.
        // INDIRECT_BUFFER: several StorageBuffer tenants double as indirect
        // argument sources (the ShaderDebugDraw channels ARE their own
        // DrawArraysIndirect args; the virtual-geometry command/args buffers
        // feed vkCmdDrawIndexedIndirectCount; GPU particles' indirect-draw
        // SSBO) — #691 Phase 7 Wave C. Costs nothing on buffers never drawn
        // from.
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        // DynamicDraw means "CPU writes, GPU reads": let VMA prefer a
        // host-writable (BAR/host-visible) placement when one exists, falling
        // back to device-local + a transfer path otherwise. DynamicCopy is
        // GPU-writes/GPU-reads and stays pure device-local.
        if (m_Usage == StorageBufferUsage::DynamicDraw)
        {
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        VmaAllocationInfo outInfo{};
        VulkanUpload::VkCheck(vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &m_Buffer, &m_Allocation, &outInfo),
                              "vmaCreateBuffer (VulkanStorageBuffer)");
        vmaSetAllocationName(device->GetAllocator(), m_Allocation, "VulkanStorageBuffer");

        m_Mapped = nullptr;
        m_NeedsFlush = false;
        VkMemoryPropertyFlags memProps = 0;
        vmaGetAllocationMemoryProperties(device->GetAllocator(), m_Allocation, &memProps);
        if ((memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            m_Mapped = outInfo.pMappedData;
            m_NeedsFlush = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
        }

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);

        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, VulkanUpload::VkHandleToU64(m_Buffer), RHI::Backend::Vulkan);
        // Root-object registration so the dispatch path can resolve a
        // BindStorageBuffer packet's handle back to this object (#691
        // Phase 7). Identity is preserved across Resize (Sync), so
        // re-registering the same key just refreshes the same entry.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::StorageBuffer, this);
        // GL twins occupy their binding point from creation (glBindBufferBase
        // in the ctor); mirror that so a pass that never calls Bind() still
        // resolves.
        VulkanBindingState::Get().SetStorageBuffer(m_Binding, this);
    }

    void VulkanStorageBuffer::ReleaseBuffer()
    {
        if (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
        }
    }

    void VulkanStorageBuffer::Bind() const
    {
        // No driver bind — the address travels in root data (ADR 0011 §4).
        // What Bind() DOES mean here is GL's glBindBufferBase semantics:
        // publish this buffer as binding point m_Binding's occupant so the
        // draw-time root writer can find it.
        VulkanBindingState::Get().SetStorageBuffer(m_Binding, const_cast<VulkanStorageBuffer*>(this));
    }

    void VulkanStorageBuffer::Unbind() const
    {
        auto& state = VulkanBindingState::Get();
        if (state.GetStorageBuffer(m_Binding) == this)
        {
            state.SetStorageBuffer(m_Binding, nullptr);
        }
    }

    void VulkanStorageBuffer::SetData(const void* data, u32 size, u32 offset)
    {
        OLO_PROFILE_FUNCTION();

        if (data == nullptr || size == 0)
        {
            return;
        }
        if (offset + size > std::max(m_Size, 1u))
        {
            OLO_CORE_ERROR("VulkanStorageBuffer::SetData: {}+{} exceeds the buffer's {} bytes — dropping", offset,
                           size, m_Size);
            return;
        }

        // NOTE (in-flight caveat, same as VulkanVertexBuffer): a direct
        // mapped write races a PREVIOUS frame's submitted reads of the same
        // range. Current DynamicDraw call sites write at load/setup time or
        // rewrite whole per-frame ranges whose consumers are recorded after
        // the write; a streaming ring lands with the wave that needs it.
        if (m_Mapped != nullptr)
        {
            std::memcpy(static_cast<u8*>(m_Mapped) + offset, data, size);
            if (m_NeedsFlush)
            {
                vmaFlushAllocation(VulkanDevice::Get()->GetAllocator(), m_Allocation, offset, size);
            }
        }
        else
        {
            VulkanOneShot::UploadToBuffer(m_Buffer, offset, data, size, "VulkanStorageBuffer::SetData");
        }

        // Command-ordered draw reads (#691 Phase 8): draws recorded after
        // this write must see THESE bytes even though the persistent buffer
        // keeps getting overwritten until submit. See GetRootDataAddress.
        PushSnapshot(data, size, offset);
    }

    void VulkanStorageBuffer::PushSnapshot(const void* data, u32 size, u32 offset)
    {
        // Only a write that lands between recorded draws needs command-
        // ordering; outside a recording bracket (load time, scene opens
        // between frames) the persistent write-through IS the ordered value,
        // and snapshotting every setup upload would burn frame-arena space.
        // Live-object probe, not the static flag — same rule as ClearData.
        if (VulkanUpload::TryGetRecordingVulkanAPI() == nullptr)
        {
            InvalidateSnapshot();
            return;
        }

        auto& arena = VulkanFrameArena::Get();
        const u64 generation = arena.GetFrameGeneration();
        const bool liveSnapshot = m_SnapshotAddress != 0 && m_SnapshotFrameGeneration == generation;

        // Bytes the new snapshot must carry: everything written so far this
        // frame (a shader may read any prefix the draw's instance count
        // covers), never less than a live snapshot already promised.
        const u32 newBytes = std::max(offset + size, liveSnapshot ? m_SnapshotBytes : 0u);

        // A write that does not start at 0 needs prefix bytes [0, offset)
        // from somewhere CPU-readable: the live snapshot, or the mapped
        // persistent buffer (write-combined — a slow read, but no hot path
        // writes partial ranges). A staged (non-mapped) buffer with no live
        // snapshot cannot supply them: drop the snapshot and let draws read
        // the persistent buffer, which is the pre-snapshot behaviour.
        const void* prefixSource = liveSnapshot ? m_SnapshotCpu : m_Mapped;
        if (offset > 0 && prefixSource == nullptr)
        {
            static bool s_WarnedPrefix = false;
            if (!s_WarnedPrefix)
            {
                s_WarnedPrefix = true;
                OLO_CORE_WARN("[RHI/Vulkan] VulkanStorageBuffer::SetData(offset {}) on a staged buffer with no "
                              "live snapshot — draw reads fall back to last-write-wins ordering (warn-once)",
                              offset);
            }
            InvalidateSnapshot();
            return;
        }

        // std430 block alignment: 16 covers any scalar/vector/matrix start.
        const auto allocation = arena.Allocate(newBytes, 16);
        if (!allocation.IsValid())
        {
            static bool s_WarnedOverflow = false;
            if (!s_WarnedOverflow)
            {
                s_WarnedOverflow = true;
                OLO_CORE_WARN("[RHI/Vulkan] VulkanStorageBuffer snapshot dropped — frame arena overflow "
                              "({} bytes); draw reads fall back to last-write-wins ordering (warn-once)",
                              newBytes);
            }
            InvalidateSnapshot();
            return;
        }

        auto* dst = static_cast<u8*>(allocation.Cpu);
        if (offset > 0)
        {
            // A live snapshot may be SHORTER than this write's offset — clamp
            // the prefix to what it actually holds and zero the gap (bytes no
            // writer defined this frame). The mapped persistent buffer always
            // covers the validated offset, so its copy stays whole.
            const u64 prefixAvailable = liveSnapshot ? std::min<u64>(offset, m_SnapshotBytes) : offset;
            std::memcpy(dst, prefixSource, prefixAvailable);
            if (prefixAvailable < offset)
            {
                std::memset(dst + prefixAvailable, 0, offset - prefixAvailable);
            }
        }
        std::memcpy(dst + offset, data, size);
        if (const u32 writtenEnd = offset + size; writtenEnd < newBytes)
        {
            // Tail beyond this write: carry the live snapshot's remainder so
            // earlier-promised content survives, else zero-fill (reads past
            // the written range were never defined by any writer this frame).
            if (liveSnapshot)
            {
                std::memcpy(dst + writtenEnd, static_cast<const u8*>(m_SnapshotCpu) + writtenEnd,
                            newBytes - writtenEnd);
            }
            else
            {
                std::memset(dst + writtenEnd, 0, newBytes - writtenEnd);
            }
        }
        arena.FlushWrite(allocation, newBytes);

        m_SnapshotFrameGeneration = generation;
        m_SnapshotAddress = allocation.Gpu;
        m_SnapshotCpu = allocation.Cpu;
        m_SnapshotBytes = newBytes;
    }

    VkDeviceAddress VulkanStorageBuffer::GetRootDataAddress()
    {
        if (m_SnapshotAddress != 0 && m_SnapshotFrameGeneration == VulkanFrameArena::Get().GetFrameGeneration())
        {
            return m_SnapshotAddress;
        }
        return m_DeviceAddress;
    }

    void VulkanStorageBuffer::GetData(void* outData, u32 size, u32 offset) const
    {
        OLO_PROFILE_FUNCTION();

        if (outData == nullptr || size == 0)
        {
            return;
        }
        if (offset + size > std::max(m_Size, 1u))
        {
            OLO_CORE_ERROR("VulkanStorageBuffer::GetData: {}+{} exceeds the buffer's {} bytes", offset, size, m_Size);
            std::memset(outData, 0, size);
            return;
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Buffer == VK_NULL_HANDLE)
        {
            std::memset(outData, 0, size);
            return;
        }

        // Mid-frame (#691 Phase 8): the producing dispatch may still sit
        // unsubmitted in the frame command buffer, and queue submissions
        // execute in submit order — the one-shot below would read the
        // PREVIOUS frame's contents (plus a full GPU stall for nothing).
        // Submit-and-continue the frame first; a refused flush (headless
        // recording, backbuffer already written, open query) falls back to
        // the old behaviour with a warn-once, never silently.
        // Live-object probe, not the static flag — see SubImage's note.
        if (VulkanUpload::TryGetRecordingVulkanAPI() != nullptr)
        {
            auto* context = VulkanContext::Get();
            if (context == nullptr || !context->FlushFrameRecordingAndWait())
            {
                static bool s_Warned = false;
                if (!s_Warned)
                {
                    s_Warned = true;
                    OLO_CORE_WARN("[Vulkan] mid-frame StorageBuffer::GetData without a frame flush — the "
                                  "readback may return the previous frame's contents");
                }
            }
        }

        // Always a one-shot copy, even on a mapped placement: a DynamicCopy
        // buffer's contents come from GPU writes, and the copy's barrier is
        // what makes those available to the host read.
        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = size;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo readbackAlloc{};
        readbackAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        readbackAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo readbackOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &readbackInfo, &readbackAlloc, &readback, &readbackAllocation,
                            &readbackOut) != VK_SUCCESS)
        {
            std::memset(outData, 0, size);
            return;
        }

        const bool ok = VulkanOneShot::Submit(
            "VulkanStorageBuffer::GetData",
            [&](VkCommandBuffer cmd)
            {
                // Make any prior GPU writes to the source range available to
                // the copy first.
                VkBufferMemoryBarrier2 pre{};
                pre.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                pre.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                pre.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                pre.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                pre.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                pre.buffer = m_Buffer;
                pre.offset = offset;
                pre.size = size;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.bufferMemoryBarrierCount = 1u;
                dep.pBufferMemoryBarriers = &pre;
                vkCmdPipelineBarrier2(cmd, &dep);

                VkBufferCopy region{};
                region.srcOffset = offset;
                region.dstOffset = 0;
                region.size = size;
                vkCmdCopyBuffer(cmd, m_Buffer, readback, 1u, &region);
            });

        if (ok)
        {
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, size);
            std::memcpy(outData, readbackOut.pMappedData, size);
        }
        else
        {
            std::memset(outData, 0, size);
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
    }

    void VulkanStorageBuffer::ClearData()
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Buffer == VK_NULL_HANDLE || m_Size == 0)
        {
            return;
        }

        // A clear supersedes any command-ordered snapshot: draws recorded
        // after it must observe zeros (persistent buffer), not the pre-clear
        // snapshot bytes. No-op for the GPU-written tenants below, which
        // never SetData mid-frame.
        InvalidateSnapshot();

        // Mid-frame (#691 Phase 8): a ClearData between two GPU uses
        // (ToneMap's exposure-reset shape, the fluid solver's grid-head
        // clears) must be ORDERED within the frame command buffer — both the
        // one-shot below (submits BEFORE the still-recording frame) and the
        // mapped memset (a CPU write the frame's earlier-recorded dispatches
        // would observe at submit time) break that ordering. Route through
        // the facade's frame-CB fill, which ends the rendering scope and
        // brackets the fill with the right barriers. This check deliberately
        // PRECEDES the mapped fast path.
        // Live-object probe, not the static flag — see SubImage's note.
        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            vk->ClearBufferUInt(m_RHIHandle.Get(), 0u);
            return;
        }

        if (m_Mapped != nullptr)
        {
            std::memset(m_Mapped, 0, m_Size);
            if (m_NeedsFlush)
            {
                vmaFlushAllocation(device->GetAllocator(), m_Allocation, 0, m_Size);
            }
            return;
        }

        // Load-time/one-shot fill (no frame recording live, so submit order
        // cannot invert anything).
        VulkanOneShot::Submit("VulkanStorageBuffer::ClearData",
                              [&](VkCommandBuffer cmd)
                              {
                                  vkCmdFillBuffer(cmd, m_Buffer, 0, VK_WHOLE_SIZE, 0u);

                                  VkBufferMemoryBarrier2 post{};
                                  post.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                                  post.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                                  post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                                  post.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                                  post.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                                  post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                  post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                  post.buffer = m_Buffer;
                                  post.offset = 0;
                                  post.size = VK_WHOLE_SIZE;
                                  VkDependencyInfo dep{};
                                  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                                  dep.bufferMemoryBarrierCount = 1u;
                                  dep.pBufferMemoryBarriers = &post;
                                  vkCmdPipelineBarrier2(cmd, &dep);
                              });
    }

    void VulkanStorageBuffer::Resize(u32 newSize)
    {
        OLO_PROFILE_FUNCTION();

        if (newSize == m_Size && m_Buffer != VK_NULL_HANDLE)
        {
            return;
        }

        // Same contract as the GL twin: Resize invalidates existing data.
        ReleaseBuffer();
        m_Size = newSize;
        // Sync inside CreateBuffer PRESERVES the identity — same object, new
        // storage.
        CreateBuffer();
        // Resize invalidates content; a stale snapshot must not outlive it.
        InvalidateSnapshot();
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
