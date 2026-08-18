#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GPUCache/GPUBufferLock.h"

#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        constexpr u64 kNanosecondsPerSecond = 1'000'000'000ull;
        // How long each ClientWaitFence call blocks before we re-check.
        constexpr u64 kFenceWaitChunkNanoseconds = kNanosecondsPerSecond;
        // Re-log every N chunks so a never-signalling fence stays loud.
        constexpr u32 kFenceWaitLogEveryChunks = 5;
    } // namespace

    GPUBufferLockManager::~GPUBufferLockManager()
    {
        Reset();
    }

    void GPUBufferLockManager::WaitForLockedRange(sizet lockBeginBytes, sizet lockLengthBytes)
    {
        const GPUBufferRange testRange{ lockBeginBytes, lockLengthBytes };

        auto overlapping = std::partition(m_BufferLocks.begin(), m_BufferLocks.end(),
                                          [&testRange](const BufferLock& lock)
                                          { return !testRange.Overlaps(lock.m_Range); });
        for (auto it = overlapping; it != m_BufferLocks.end(); ++it)
        {
            Wait(it->m_Fence);
            RenderCommand::DestroyFence(it->m_Fence);
        }
        m_BufferLocks.erase(overlapping, m_BufferLocks.end());
    }

    void GPUBufferLockManager::LockRange(sizet lockBeginBytes, sizet lockLengthBytes)
    {
        // Reap locks whose fences have already signaled (GPU work done — the
        // range is safe to rewrite, so the lock carries no information).
        // Without this, a ring that wraps rarely accumulates one live GLsync
        // per Commit until the head comes back around — thousands of driver
        // sync objects, and an ever-longer overlap scan in WaitForLockedRange.
        std::erase_if(m_BufferLocks,
                      [](const BufferLock& lock)
                      {
                          if (RenderCommand::IsFenceSignaled(lock.m_Fence))
                          {
                              RenderCommand::DestroyFence(lock.m_Fence);
                              return true;
                          }
                          return false;
                      });

        const u64 fence = RenderCommand::CreateFence();
        m_BufferLocks.push_back({ { lockBeginBytes, lockLengthBytes }, fence });
    }

    void GPUBufferLockManager::Reset()
    {
        // Guarded so a destructor reached at static teardown (rendering device
        // already gone) degrades to leaking the fences — as the pre-#704 code
        // did — instead of calling into a dead RendererAPI.
        if (!m_BufferLocks.empty() && RenderCommand::IsDeviceAvailable())
        {
            for (const BufferLock& lock : m_BufferLocks)
            {
                RenderCommand::DestroyFence(lock.m_Fence);
            }
        }
        m_BufferLocks.clear();
    }

    void GPUBufferLockManager::Wait(u64 fence) const
    {
        // ClientWaitFence flushes on the OpenGL backend (GL_SYNC_FLUSH_COMMANDS_BIT
        // inside the wait), so a fence whose commands were never submitted still
        // completes. NOTE for a future Vulkan enablement: VulkanRendererAPI stages
        // its CreateFence signal for the NEXT queue submit, so an intra-frame wait
        // with no submit in between would spin here — currently unreachable
        // (Vulkan's AllocatePersistentUploadStorage is a stub returning null, so
        // no persistent-mapped consumer of this class can exist there), but audit
        // this loop when that stub is implemented. The periodic error keeps any
        // such regression loud instead of a silent hang.
        u32 timeouts = 0;
        while (true)
        {
            const RHI::FenceStatus status = RenderCommand::ClientWaitFence(fence, kFenceWaitChunkNanoseconds);
            if (status == RHI::FenceStatus::AlreadySignaled || status == RHI::FenceStatus::ConditionSatisfied)
            {
                return;
            }
            if (status == RHI::FenceStatus::Failed)
            {
                OLO_CORE_WARN("GPUBufferLockManager: ClientWaitFence failed — treating the range as unlocked");
                return;
            }
            // Periodic, not once: a fence that never signals must keep saying so,
            // or the "loud instead of silent" guarantee above lasts one message.
            if (++timeouts % kFenceWaitLogEveryChunks == 0)
            {
                OLO_CORE_ERROR("GPUBufferLockManager: a range fence has not signaled after {} seconds — "
                               "still waiting (GPU stall or a backend that never submitted the fence?)",
                               (static_cast<u64>(timeouts) * kFenceWaitChunkNanoseconds) / kNanosecondsPerSecond);
            }
        }
    }
} // namespace OloEngine
