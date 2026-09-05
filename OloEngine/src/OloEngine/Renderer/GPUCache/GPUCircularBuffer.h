#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GPUCache/GPUBufferLock.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    // @brief Fence-locked persistent-mapped upload ring (issue #704, ported
    // from the VoxelEngine reference's GPUCircularBuffer).
    //
    // The classic streaming pattern: the CPU writes into a persistent-mapped
    // buffer at a moving head, the GPU copies/reads from it, and fence range
    // locks keep the writer from scribbling over bytes a still-executing
    // command needs. Byte-granular on purpose — its one engine consumer stages
    // heterogeneous payloads (VirtualMeshRegistry page uploads).
    //
    // Usage per staged payload, on the render thread:
    //   1. u64 offset; u8* dst = ring.Reserve(bytes, offset);   // waits on overlapping fences
    //   2. memcpy(dst, payload, bytes);
    //   3. enqueue the GPU command that consumes [offset, offset+bytes)
    //   4. ring.Commit(bytes);                                  // fences the range, advances head
    //
    // A Reserve that cannot fit before the end of the buffer wraps to 0 (the
    // remainder tail is skipped, never split). A payload larger than the whole
    // ring returns nullptr — the caller falls back to a direct upload.
    class GPUCircularBuffer
    {
      public:
        GPUCircularBuffer() = default;
        ~GPUCircularBuffer()
        {
            Destroy();
        }
        GPUCircularBuffer(const GPUCircularBuffer&) = delete;
        GPUCircularBuffer& operator=(const GPUCircularBuffer&) = delete;

        [[nodiscard]] bool Create(u64 sizeBytes)
        {
            if (m_SizeBytes != 0 || sizeBytes == 0 || !RenderCommand::IsDeviceAvailable())
            {
                return false;
            }
            m_Buffer = RenderCommand::CreateBufferHandle();
            m_MappedPtr = static_cast<u8*>(RenderCommand::AllocatePersistentUploadStorage(m_Buffer, sizeBytes));
            if (m_MappedPtr == nullptr)
            {
                RenderCommand::DeleteBuffer(m_Buffer);
                m_Buffer = RHI::NullResource;
                return false;
            }
            m_SizeBytes = sizeBytes;
            m_Head = 0;
            m_ReservedBytes = 0;
            return true;
        }

        void Destroy()
        {
            m_LockManager.Reset();
            if (m_Buffer.IsValid())
            {
                // Device guard: this destructor can be reached at static
                // teardown through an owner whose explicit Shutdown was
                // skipped (VirtualMeshRegistry is a function-local static).
                // With the RendererAPI gone, leak — the pre-#704 hand-rolled
                // ring leaked on that path too — rather than call into it.
                if (RenderCommand::IsDeviceAvailable())
                {
                    if (m_MappedPtr != nullptr)
                    {
                        RenderCommand::UnmapBuffer(m_Buffer);
                    }
                    RenderCommand::DeleteBuffer(m_Buffer);
                }
                m_MappedPtr = nullptr;
                m_Buffer = RHI::NullResource;
            }
            m_SizeBytes = 0;
            m_Head = 0;
            m_ReservedBytes = 0;
        }

        // Returns a write pointer for `bytes` contiguous bytes and the ring
        // offset it corresponds to, waiting out any fence still covering that
        // range. Wraps to offset 0 when the tail cannot fit. nullptr when
        // `bytes` exceeds the ring (or the ring was never created).
        //
        // An uncommitted reservation is simply superseded by the next Reserve —
        // the head has not moved, so nothing is lost.
        [[nodiscard]] u8* Reserve(u64 bytes, u64& outOffset)
        {
            if (m_MappedPtr == nullptr || bytes == 0 || bytes > m_SizeBytes)
            {
                return nullptr;
            }
            if (m_Head + bytes > m_SizeBytes)
            {
                // Wrapping means waiting on a range this FRAME already fenced.
                // That only terminates on a backend whose fence can complete
                // without a queue submit: OpenGL's ClientWaitFence flushes
                // inside the wait, Vulkan STAGES the signal for the next submit
                // and would spin forever (issue #1052 — the audit
                // GPUBufferLockManager::Wait asks for, now that Vulkan's
                // AllocatePersistentUploadStorage is real). Refuse the
                // reservation instead; the caller's direct-upload path is a
                // complete lowering on both backends, so this costs staging
                // bandwidth, never correctness.
                if (!RenderCommand::SupportsIntraFrameFenceCompletion())
                {
                    static bool s_Warned = false;
                    if (!s_Warned)
                    {
                        s_Warned = true;
                        OLO_CORE_WARN("GPUCircularBuffer: a {}-byte reservation would wrap the {}-byte ring, and this "
                                      "backend cannot complete a fence within the frame — falling back to direct "
                                      "uploads for the rest of this batch (issue #1052)",
                                      bytes, m_SizeBytes);
                    }
                    return nullptr;
                }
                m_Head = 0;
            }
            m_LockManager.WaitForLockedRange(m_Head, bytes);
            outOffset = m_Head;
            m_ReservedBytes = bytes;
            return m_MappedPtr + m_Head;
        }

        // Fences the range handed out by the matching Reserve and advances the
        // head past it. Call AFTER enqueueing the GPU command that consumes it.
        //
        // `bytes` must equal the outstanding reservation: the fence and the head
        // advance both cover the range Reserve handed out, so committing a
        // SHORTER length would leave the tail of that range unfenced and
        // re-handable while the GPU is still reading it. The reservation length
        // is therefore authoritative and `bytes` is the cross-check.
        void Commit(u64 bytes)
        {
            OLO_CORE_ASSERT(m_ReservedBytes != 0, "GPUCircularBuffer::Commit without a matching Reserve");
            OLO_CORE_ASSERT(bytes == m_ReservedBytes,
                            "GPUCircularBuffer::Commit length differs from the reserved length");
            if (m_ReservedBytes == 0)
            {
                return;
            }
            OLO_CORE_ASSERT(m_Head + m_ReservedBytes <= m_SizeBytes,
                            "GPUCircularBuffer::Commit past the end of the ring");
            m_LockManager.LockRange(m_Head, m_ReservedBytes);
            m_Head += m_ReservedBytes;
            m_ReservedBytes = 0;
            if (m_Head == m_SizeBytes)
            {
                m_Head = 0;
            }
        }

        [[nodiscard]] u64 GetSize() const
        {
            return m_SizeBytes;
        }
        [[nodiscard]] u64 GetHead() const
        {
            return m_Head;
        }
        [[nodiscard]] bool IsCreated() const
        {
            return m_SizeBytes != 0;
        }
        [[nodiscard]] RHI::ResourceHandle GetDeviceHandle() const
        {
            return m_Buffer;
        }

      private:
        GPUBufferLockManager m_LockManager;
        RHI::ResourceHandle m_Buffer{};
        u8* m_MappedPtr = nullptr;
        u64 m_SizeBytes = 0;
        u64 m_Head = 0;
        u64 m_ReservedBytes = 0; // outstanding Reserve, cleared by Commit
    };
} // namespace OloEngine
