#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanStorageBuffer.h"
#include "Platform/Vulkan/VulkanQueueSelection.h"
#include "Platform/Vulkan/VulkanRecordingContext.h"

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
#include <atomic>
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
            // Both mirrors — see ~VulkanUniformBuffer: clearing only the
            // calling thread's leaves the process-wide one dangling.
            VulkanBindingState::Get().ClearBuffer(this);
            VulkanBindingState::Global().ClearBuffer(this);
            VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            ReleaseBuffer();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanStorageBuffer: release failed ({}) — leaking the buffer until process exit", e.what());
        }
        catch (...)
        {
            // The catch-all is not belt-and-braces: `catch (const
            // std::exception&)` alone lets anything not derived from it escape
            // and terminate. One policy across every Vulkan resource
            // destructor (#803).
            OLO_CORE_ERROR("~VulkanStorageBuffer: release failed (unknown exception) — leaking the buffer until "
                           "process exit");
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
        // SHADER_DEVICE_ADDRESS: the root-data model (ADR 0011 §4)
        // addresses buffers by VkDeviceAddress embedded in the root struct, so
        // every storage buffer must be addressable. bufferDeviceAddress is
        // enabled at device creation and the VMA allocator carries
        // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT.
        // INDIRECT_BUFFER: several StorageBuffer tenants double as indirect
        // argument sources (the ShaderDebugDraw channels ARE their own
        // DrawArraysIndirect args; the virtual-geometry command/args buffers
        // feed vkCmdDrawIndexedIndirectCount; GPU particles' indirect-draw
        // SSBO) — #691. Costs nothing on buffers never drawn
        // from.
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // #808: a buffer both queues can reach must be CONCURRENT — the set a
        // compute dispatch touches is not statically known on a bindless
        // backend, so no ownership transfer can cover it. No-op without an
        // async compute queue. See VulkanQueueSelection.h.
        const VulkanQueueSelection::CrossQueueBufferSharing crossQueueSharing;
        crossQueueSharing.ApplyTo(bufferInfo);

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
        // BindStorageBuffer packet's handle back to this object (#691).
        // Identity is preserved across Resize (Sync), so
        // re-registering the same key just refreshes the same entry.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::StorageBuffer, this);
        // GL twins occupy their binding point from creation (glBindBufferBase
        // in the ctor); mirror that so a pass that never calls Bind() still
        // resolves. A by-address buffer (kNoBinding) publishes nowhere.
        if (m_Binding != StorageBuffer::kNoBinding)
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
        if (m_Binding != StorageBuffer::kNoBinding)
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
        // Amendment (92) rule 6, checked at record time: one writer per object
        // per region. A refused item skips the write in every build — the
        // mapped memcpy and PushSnapshot below are the race.
        if (!ClaimParallelWriter(m_ParallelWriter, "storage buffer"))
        {
            return;
        }

        if (data == nullptr || size == 0)
        {
            return;
        }
        // Widened BEFORE the sum: both operands are u32, so `offset + size`
        // is evaluated modulo 2^32 and offset=0xFFFFFF00, size=0x200 wraps to
        // 0x100 — passing the very guard meant to stop it, then handing the
        // wrapped range to the mapped memcpy below (a heap write far outside
        // the mapping) or to VulkanOneShot::UploadToBuffer.
        // VulkanTexture2D::SubImage already compares against the remaining
        // extent for the same reason.
        if (static_cast<u64>(offset) + static_cast<u64>(size) > static_cast<u64>(std::max(m_Size, 1u)))
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

        // Command-ordered draw reads (#691): draws recorded after
        // this write must see THESE bytes even though the persistent buffer
        // keeps getting overwritten until submit. See GetRootDataAddress.
        PushSnapshot(data, size, offset);
    }

    namespace
    {
        // Process-wide because the condition is a property of the frame's
        // arena budget and of call-site shapes, not of one buffer — the same
        // scope the warn-once flags beside it already have.
        std::atomic<u64> s_SnapshotRefusedCount{ 0 };
    } // namespace

    u64 VulkanStorageBuffer::GetSnapshotRefusedCount()
    {
        return s_SnapshotRefusedCount.load(std::memory_order_relaxed);
    }

    void VulkanStorageBuffer::PushSnapshot(const void* data, u32 size, u32 offset)
    {
        // A GPU-PRODUCED buffer never snapshots (issue #1058). DynamicCopy
        // means "GPU writes, GPU reads": its authoritative producer is a
        // compute dispatch, which resolves GetDeviceAddress and therefore
        // writes the PERSISTENT allocation. A CPU SetData on such a buffer is
        // a seed or a zero-init in front of that dispatch, never a value a
        // draw is meant to read command-ordered — so versioning it into the
        // frame arena does not order two CPU writes, it SPLITS the buffer in
        // two: dispatches write persistent while every later draw reads a CPU
        // snapshot that the dispatch's results can never reach.
        //
        // That is exactly how virtual geometry's mesh-shader arm went dark.
        // VirtualMeshRegistry::PrepareFrame zeroes the draw-args buffer before
        // the cull dispatches; the cull wrote the real DrawCount into the
        // persistent buffer; the task stage is a DRAW, so it read the CPU's
        // zero snapshot and issued EmitMeshTasksEXT(0) — a legal launch into
        // an empty frame, with no dropped draw, no stub and no validation
        // error to name it. (The MDI arm reads its count through the indirect
        // parameter VkBuffer and the software raster is a dispatch, so both
        // read persistent and both stayed correct.)
        //
        // StorageBuffer::ClearData(offset, size) exists for the same hazard one
        // level up (#1015) and stays the better call site-side: a fill inside
        // the command stream needs no CPU staging at all. This guard is what
        // makes a plain SetData safe on the buffers that did not get that
        // treatment.
        if (m_Usage == StorageBufferUsage::DynamicCopy)
        {
            InvalidateSnapshot();
            return;
        }

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
        // Invariant since #1080: every snapshot this function publishes is
        // whole-buffer, and Resize drops the snapshot alongside the storage,
        // so a LIVE snapshot always measures exactly m_Size.
        OLO_CORE_ASSERT(!liveSnapshot || m_SnapshotBytes == m_Size,
                        "VulkanStorageBuffer: live snapshot is not whole-buffer");

        // Nothing has read this snapshot yet: rewrite it in place. A snapshot
        // is only observable through the address a DRAW embedded, so while
        // m_SnapshotConsumed is false no recorded work can tell the difference
        // between "overwritten" and "never staged" — and command ordering is
        // defined against recorded draws, not against SetData calls.
        //
        // This is what keeps whole-buffer snapshots affordable. GPUScene::Upload
        // issues one SetData per NON-ADJACENT dirty range (CoalesceDirtyRanges
        // merges only strictly adjacent indices), so a frame that dirties N
        // scattered records would otherwise claim N whole-buffer arena ranges —
        // and the frame arena is where every draw's root data lives, so
        // exhausting it drops root data for the WHOLE frame, not just for this
        // buffer. With reuse, that batch costs one whole-buffer fill plus N
        // small memcpys.
        if (liveSnapshot && !m_SnapshotConsumed.load(std::memory_order_relaxed))
        {
            std::memcpy(static_cast<u8*>(m_SnapshotCpu) + offset, data, size);
            arena.FlushWrite(VulkanFrameArenaAllocation{ m_SnapshotCpu, m_SnapshotAddress, m_SnapshotArenaOffset },
                             m_SnapshotBytes);
            return;
        }

        // A snapshot covers the WHOLE buffer, or there is no snapshot (#1080).
        // GetRootDataAddress hands the draw a bare device address with no
        // length attached, so the shader's own indexing is the only bound: a
        // read past the snapshot's end lands on whatever the frame arena
        // handed out next, not on this buffer's bytes. Sizing the snapshot to
        // the write (an 11,264-byte binding-17 buffer got a 176-byte snapshot)
        // makes that an out-of-bounds device read under buffer-device-address
        // root data, not a wrong pixel. The sibling VulkanUniformBuffer has
        // always pushed its whole shadow for exactly this reason.
        const u32 newBytes = m_Size;

        // Bytes this write does not define — the prefix [0, offset) and the
        // tail [offset + size, m_Size) — have to come from somewhere CPU-
        // readable: the live snapshot (whole-buffer by the rule above), or the
        // mapped persistent buffer (write-combined — a slow read, but no hot
        // path writes partial ranges). A staged (non-mapped) buffer with no
        // live snapshot cannot supply them: drop the snapshot and let draws
        // read the persistent buffer, which is the pre-snapshot behaviour.
        // Zero-filling the gap instead would be worse than either — it hands
        // the shader defined-looking bytes that no writer ever wrote.
        // ...and m_Mapped only counts while it still tells the truth. A GPU-side
        // write recorded earlier this frame (a mid-frame ClearData, an external
        // UploadBufferSubData / CopyBufferSubData) executes at submit and never
        // touches the host mapping, so filling from it would resurrect the very
        // bytes that write was issued to replace — a cleared range reappearing
        // for every draw recorded after the next partial SetData.
        const void* fillSource = liveSnapshot ? m_SnapshotCpu : (GpuWroteThisFrame() ? nullptr : m_Mapped);
        const bool coversWholeBuffer = offset == 0 && size == m_Size;
        if (!coversWholeBuffer && fillSource == nullptr)
        {
            static std::atomic<bool> s_WarnedPartial{ false };
            if (!s_WarnedPartial.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] VulkanStorageBuffer::SetData({}+{} of {} B) on a staged buffer with no "
                              "live snapshot — cannot source the undefined bytes, so draw reads fall back to "
                              "last-write-wins ordering (warn-once)",
                              offset, size, m_Size);
            }
            s_SnapshotRefusedCount.fetch_add(1, std::memory_order_relaxed);
            InvalidateSnapshot();
            return;
        }

        // std430 block alignment: 16 covers any scalar/vector/matrix start.
        const auto allocation = arena.Allocate(newBytes, 16);
        if (!allocation.IsValid())
        {
            static std::atomic<bool> s_WarnedOverflow{ false };
            if (!s_WarnedOverflow.exchange(true, std::memory_order_relaxed))
            {
                OLO_CORE_WARN("[RHI/Vulkan] VulkanStorageBuffer snapshot dropped — frame arena overflow "
                              "({} bytes); draw reads fall back to last-write-wins ordering (warn-once)",
                              newBytes);
            }
            s_SnapshotRefusedCount.fetch_add(1, std::memory_order_relaxed);
            InvalidateSnapshot();
            return;
        }

        auto* dst = static_cast<u8*>(allocation.Cpu);
        if (offset > 0)
        {
            std::memcpy(dst, fillSource, offset);
        }
        std::memcpy(dst + offset, data, size);
        if (const u32 writtenEnd = offset + size; writtenEnd < newBytes)
        {
            std::memcpy(dst + writtenEnd, static_cast<const u8*>(fillSource) + writtenEnd, newBytes - writtenEnd);
        }
        arena.FlushWrite(allocation, newBytes);

        m_SnapshotFrameGeneration = generation;
        m_SnapshotAddress = allocation.Gpu;
        m_SnapshotCpu = allocation.Cpu;
        m_SnapshotArenaOffset = allocation.Offset;
        m_SnapshotConsumed.store(false, std::memory_order_relaxed);
        m_SnapshotBytes = newBytes;
    }

    VkDeviceAddress VulkanStorageBuffer::GetRootDataAddress()
    {
        if (!HasLiveSnapshot())
        {
            return m_DeviceAddress;
        }
        // This draw is about to embed the snapshot's address, so its contents
        // become observable from here on: a later SetData in the same frame
        // must stage a NEW range rather than rewrite this one (see
        // m_SnapshotConsumed).
        m_SnapshotConsumed.store(true, std::memory_order_relaxed);
        return m_SnapshotAddress;
    }

    bool VulkanStorageBuffer::HasLiveSnapshot() const
    {
        return m_SnapshotAddress != 0 && m_SnapshotFrameGeneration == VulkanFrameArena::Get().GetFrameGeneration();
    }

    void VulkanStorageBuffer::NoteGpuWriteThisFrame()
    {
        m_GpuWriteFrameGeneration = VulkanFrameArena::Get().GetFrameGeneration();
    }

    bool VulkanStorageBuffer::GpuWroteThisFrame() const
    {
        return m_GpuWriteFrameGeneration == VulkanFrameArena::Get().GetFrameGeneration();
    }

    u32 VulkanStorageBuffer::GetLiveSnapshotBytes() const
    {
        return HasLiveSnapshot() ? m_SnapshotBytes : 0u;
    }

    const void* VulkanStorageBuffer::GetLiveSnapshotCpuData() const
    {
        return HasLiveSnapshot() ? m_SnapshotCpu : nullptr;
    }

    void VulkanStorageBuffer::GetData(void* outData, u32 size, u32 offset) const
    {
        OLO_PROFILE_FUNCTION();

        if (outData == nullptr || size == 0)
        {
            return;
        }
        // Widened before the sum — same u32 wrap as SetData's guard.
        if (static_cast<u64>(offset) + static_cast<u64>(size) > static_cast<u64>(std::max(m_Size, 1u)))
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

        // Mid-frame (#691): the producing dispatch may still sit
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
                static std::atomic<bool> s_Warned{ false };
                if (!s_Warned.exchange(true, std::memory_order_relaxed))
                {
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

        // Rule 6 covers a clear as much as a SetData: it rewrites the snapshot
        // fields and the buffer, so a second item of the region is refused here
        // too, in every build.
        if (!ClaimParallelWriter(m_ParallelWriter, "storage buffer"))
        {
            return;
        }

        // A clear supersedes any command-ordered snapshot: draws recorded
        // after it must observe zeros (persistent buffer), not the pre-clear
        // snapshot bytes. No-op for the GPU-written tenants below, which
        // never SetData mid-frame.
        InvalidateSnapshot();

        // Mid-frame (#691): a ClearData between two GPU uses
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
            // ONLY this arm marks the mapping stale. The recorded fill executes at
            // submit and never touches m_Mapped, so from here to the end of the
            // frame the host mapping would hand a later partial SetData exactly the
            // pre-clear bytes this call is removing. The mapped arm below, by
            // contrast, memsets m_Mapped itself — the mapping stays truthful there,
            // and marking it stale would refuse later snapshots (and bump the
            // refusal counter) for no reason.
            NoteGpuWriteThisFrame();
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

    void VulkanStorageBuffer::ClearData(u32 offset, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Buffer == VK_NULL_HANDLE || m_Size == 0 || size == 0)
        {
            return;
        }
        if (static_cast<u64>(offset) + size > m_Size || (offset % 4u) != 0u || (size % 4u) != 0u)
        {
            OLO_CORE_ERROR("VulkanStorageBuffer::ClearData: range {}+{} is out of the buffer's {} bytes or not 4-byte "
                           "aligned -- dropping",
                           offset, size, m_Size);
            return;
        }

        // The same three paths as the whole-buffer clear above, ranged. Rule
        // 6 and the snapshot rule apply unchanged: a ranged clear still
        // supersedes any command-ordered snapshot, and mid-frame it must be
        // ordered inside the frame command buffer.
        if (!ClaimParallelWriter(m_ParallelWriter, "storage buffer"))
        {
            return;
        }
        InvalidateSnapshot();

        if (auto* vk = VulkanUpload::TryGetRecordingVulkanAPI(); vk != nullptr)
        {
            // Same split as the whole-buffer clear: only the RECORDED fill bypasses
            // m_Mapped and so invalidates it as a fill source.
            NoteGpuWriteThisFrame();
            vk->ClearBufferUInt(m_RHIHandle.Get(), 0u, offset, size);
            return;
        }

        if (m_Mapped != nullptr)
        {
            std::memset(static_cast<u8*>(m_Mapped) + offset, 0, size);
            if (m_NeedsFlush)
            {
                vmaFlushAllocation(device->GetAllocator(), m_Allocation, offset, size);
            }
            return;
        }

        VulkanOneShot::Submit("VulkanStorageBuffer::ClearData(range)",
                              [&](VkCommandBuffer cmd)
                              {
                                  vkCmdFillBuffer(cmd, m_Buffer, offset, size, 0u);

                                  VkBufferMemoryBarrier2 post{};
                                  post.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                                  post.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                                  post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                                  post.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                                  post.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                                  post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                  post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                  post.buffer = m_Buffer;
                                  post.offset = offset;
                                  post.size = size;
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
        // Creates a buffer and enqueues the old one on the reclaim queue —
        // render-thread work (amendment (92) rule 7). A RecordParallel item
        // that outgrows its buffer must have been given the capacity before
        // the fork (ShadowRenderPass::EnsureItemResources).
        if (CurrentVulkanWorkerContext() != nullptr)
        {
            OLO_CORE_ERROR("[RHI/Vulkan] StorageBuffer::Resize({} B) from a RecordParallel item — refused; size the "
                           "item's buffer before the fork",
                           newSize);
            OLO_CORE_ASSERT(false, "StorageBuffer::Resize from a RecordParallel item");
            return;
        }

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
