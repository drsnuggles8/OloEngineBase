#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/StagedBufferReadback.h"

#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>
#include <utility>

namespace OloEngine
{
    StagedBufferReadback::~StagedBufferReadback()
    {
        Release();
    }

    StagedBufferReadback::StagedBufferReadback(StagedBufferReadback&& other) noexcept
        : m_Staging(other.m_Staging), m_Capacity(other.m_Capacity), m_Staged(other.m_Staged), m_Ring(other.m_Ring),
          m_NextSerial(other.m_NextSerial)
    {
        other.m_Staging = RHI::NullResource;
        other.m_Capacity = 0;
        other.m_Staged = false;
        other.m_Ring = {};
        other.m_NextSerial = 1;
    }

    StagedBufferReadback& StagedBufferReadback::operator=(StagedBufferReadback&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_Staging = other.m_Staging;
            m_Capacity = other.m_Capacity;
            m_Staged = other.m_Staged;
            m_Ring = other.m_Ring;
            m_NextSerial = other.m_NextSerial;
            other.m_Staging = RHI::NullResource;
            other.m_Capacity = 0;
            other.m_Staged = false;
            other.m_Ring = {};
            other.m_NextSerial = 1;
        }
        return *this;
    }

    void StagedBufferReadback::Release()
    {
        if (m_Staging.IsValid())
        {
            RenderCommand::DeleteBuffer(m_Staging);
            m_Staging = RHI::NullResource;
        }
        m_Capacity = 0;
        m_Staged = false;
        ReleaseRing();
    }

    void StagedBufferReadback::ReleaseRing()
    {
        for (RingSlot& slot : m_Ring)
        {
            if (slot.Fence != 0u)
            {
                RenderCommand::DestroyFence(slot.Fence);
            }
            if (slot.Buffer.IsValid())
            {
                RenderCommand::DeleteBuffer(slot.Buffer);
            }
            slot = {};
        }
        m_NextSerial = 1;
    }

    bool StagedBufferReadback::StageDeferred(RHI::ResourceHandle source, u64 srcOffsetBytes, u64 sizeBytes,
                                             MemoryBarrierFlags barriers)
    {
        if (!source.IsValid() || sizeBytes == 0)
        {
            return false;
        }
        RingSlot* target = nullptr;
        for (RingSlot& slot : m_Ring)
        {
            if (!slot.Outstanding)
            {
                target = &slot;
                break;
            }
        }
        if (target == nullptr)
        {
            return false;
        }

        // Grow-only per slot, as Stage() does for its one buffer. The slot is
        // not outstanding, so no copy is in flight into the buffer replaced.
        if (!target->Buffer.IsValid() || target->Capacity < sizeBytes)
        {
            if (target->Buffer.IsValid())
            {
                RenderCommand::DeleteBuffer(target->Buffer);
            }
            target->Buffer = RenderCommand::CreateBufferHandle();
            target->Capacity = 0;
            if (!target->Buffer.IsValid())
            {
                return false;
            }
            RenderCommand::AllocateBufferStorage(target->Buffer, sizeBytes, RHI::MemoryResidency::DeviceToHost);
            target->Capacity = sizeBytes;
        }

        RenderCommand::MemoryBarrier(barriers | MemoryBarrierFlags::BufferUpdate);
        RenderCommand::CopyBufferSubData(source, target->Buffer, srcOffsetBytes, 0, sizeBytes);
        // Signalled once everything submitted up to and including this copy has
        // run: on Vulkan that is the submission of the frame recording it.
        target->Fence = RenderCommand::CreateFence();
        if (target->Fence == 0u)
        {
            // No fence, no way to know when the copy has landed: the copy is
            // not handed out at all rather than read on a guess.
            OLO_CORE_ERROR("StagedBufferReadback::StageDeferred: the backend returned no fence; this copy is dropped");
            return false;
        }
        target->Bytes = sizeBytes;
        target->Serial = m_NextSerial++;
        target->Outstanding = true;
        return true;
    }

    bool StagedBufferReadback::ReadCompleted(void* dest, u64 sizeBytes)
    {
        if (dest == nullptr || sizeBytes == 0)
        {
            return false;
        }
        RingSlot* oldest = nullptr;
        for (RingSlot& slot : m_Ring)
        {
            if (slot.Outstanding && (oldest == nullptr || slot.Serial < oldest->Serial))
            {
                oldest = &slot;
            }
        }
        // In order: a later copy cannot have finished before an earlier one, and
        // handing them out one per call is what lets a per-frame reader see
        // every frame.
        if (oldest == nullptr)
        {
            return false;
        }
        // A wait of zero: a poll. Not IsFenceSignaled, which on OpenGL only
        // asks and never flushes, so a fence issued in a frame that ends
        // without a swap (a test, an offscreen tool) would never signal.
        const RHI::FenceStatus status = RenderCommand::ClientWaitFence(oldest->Fence, 0u);
        if (status != RHI::FenceStatus::AlreadySignaled && status != RHI::FenceStatus::ConditionSatisfied)
        {
            return false;
        }
        RenderCommand::ReadBufferSubData(oldest->Buffer, 0, std::min(sizeBytes, oldest->Bytes), dest);
        RenderCommand::DestroyFence(oldest->Fence);
        oldest->Fence = 0;
        oldest->Outstanding = false;
        return true;
    }

    u32 StagedBufferReadback::GetDeferredCopiesOutstanding() const
    {
        u32 outstanding = 0;
        for (const RingSlot& slot : m_Ring)
        {
            outstanding += slot.Outstanding ? 1u : 0u;
        }
        return outstanding;
    }

    void StagedBufferReadback::Stage(RHI::ResourceHandle source, u64 srcOffsetBytes, u64 sizeBytes,
                                     MemoryBarrierFlags barriers)
    {
        if (!source.IsValid() || sizeBytes == 0)
        {
            return;
        }

        // Grow-only: a readback whose size varies per frame (a probe table, a
        // proxy list) would otherwise reallocate constantly.
        if (!m_Staging.IsValid() || m_Capacity < sizeBytes)
        {
            Release();
            m_Staging = RenderCommand::CreateBufferHandle();
            if (!m_Staging.IsValid())
            {
                return;
            }
            RenderCommand::AllocateBufferStorage(m_Staging, sizeBytes, RHI::MemoryResidency::DeviceToHost);
            m_Capacity = sizeBytes;
        }

        // BufferUpdate is what orders the copy itself; the caller's `barriers`
        // carry whatever ordered the WRITES it is copying (ShaderStorage for the
        // usual atomic case). Dropping either makes the copy read a value that
        // is right most of the time.
        // Vulkan's copy supplies its own availability barriers, including on
        // the one-shot path used by synchronous diagnostics between frames.
        // A separate facade barrier there has no recording command buffer.
        if (RendererAPI::GetAPI() != RendererAPI::API::Vulkan)
            RenderCommand::MemoryBarrier(barriers | MemoryBarrierFlags::BufferUpdate);
        RenderCommand::CopyBufferSubData(source, m_Staging, srcOffsetBytes, 0, sizeBytes);
        m_Staged = true;
    }

    bool StagedBufferReadback::Read(void* dest, u64 sizeBytes)
    {
        if (!m_Staged || !m_Staging.IsValid() || dest == nullptr || sizeBytes == 0)
        {
            return false;
        }
        RenderCommand::ReadBufferSubData(m_Staging, 0, std::min(sizeBytes, m_Capacity), dest);
        m_Staged = false;
        return true;
    }
} // namespace OloEngine
