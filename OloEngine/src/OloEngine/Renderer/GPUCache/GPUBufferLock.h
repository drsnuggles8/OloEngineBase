#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    // @brief Byte range of a GPU buffer, used to track in-flight GPU reads over
    // persistent-mapped memory (issue #704, ported from the VoxelEngine
    // reference's gpu_buffer_lock.h).
    struct GPUBufferRange
    {
        sizet m_StartOffset = 0;
        sizet m_Length = 0;

        [[nodiscard]] bool Overlaps(const GPUBufferRange& rhs) const
        {
            return m_StartOffset < (rhs.m_StartOffset + rhs.m_Length) &&
                   rhs.m_StartOffset < (m_StartOffset + m_Length);
        }
    };

    // @brief Fence-based range locking for persistent-mapped buffer writes.
    //
    // LockRange inserts a fence (RenderCommand::CreateFence) covering the byte
    // range just handed to the GPU; WaitForLockedRange client-waits every fence
    // whose range overlaps the bytes about to be rewritten, so the CPU never
    // scribbles over memory a still-executing command is reading. Requires a
    // live rendering device; single-threaded (render-thread) use only, same
    // contract as the rest of the renderer.
    class GPUBufferLockManager
    {
      public:
        GPUBufferLockManager() = default;
        ~GPUBufferLockManager();
        GPUBufferLockManager(const GPUBufferLockManager&) = delete;
        GPUBufferLockManager& operator=(const GPUBufferLockManager&) = delete;

        void WaitForLockedRange(sizet lockBeginBytes, sizet lockLengthBytes);
        void LockRange(sizet lockBeginBytes, sizet lockLengthBytes);

        // Drops every outstanding fence without waiting (buffer destroyed).
        void Reset();

      private:
        struct BufferLock
        {
            GPUBufferRange m_Range;
            u64 m_Fence = 0;
        };

        void Wait(u64 fence) const;

        std::vector<BufferLock> m_BufferLocks;
    };
} // namespace OloEngine
