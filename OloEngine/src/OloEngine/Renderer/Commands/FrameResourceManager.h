#pragma once

#include "OloEngine/Core/Base.h"
#include "CommandAllocator.h"
#include "CommandBucket.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace OloEngine
{
    /**
     * @brief Double-buffered frame resource management
     *
     * This class manages two sets of command buckets and allocators,
     * allowing the CPU to build frame N+1 commands while the GPU executes frame N.
     * This hides CPU/GPU latency at the cost of increased memory usage.
     *
     * Usage:
     *   1. At frame start, call BeginFrame() to get the current frame's resources
     *   2. Submit commands to the current frame's buckets
     *   3. At frame end, call EndFrame() to mark resources as ready for GPU
     *   4. GPU execution and fencing handled internally
     *
     * Thread Safety:
     *   - BeginFrame()/EndFrame() must be called from the main thread only
     *   - Frame resources can be used by multiple threads between Begin/End
     */
    class FrameResourceManager
    {
      public:
        static constexpr u32 NUM_BUFFERED_FRAMES = 2;
        static constexpr u32 ALLOCATORS_PER_FRAME = 16; // Per-worker allocators

        struct FrameResources
        {
            std::vector<std::unique_ptr<CommandAllocator>> Allocators;
            u32 AllocatorIndex = 0; // Next allocator to assign

            // GPU fence for synchronization
            u64 FenceId = 0; // Changed from u32 to u64 to avoid pointer truncation on 64-bit systems
            bool FenceSignaled = true;

            void Reset()
            {
                for (auto& alloc : Allocators)
                {
                    if (alloc)
                        alloc->Reset();
                }
                AllocatorIndex = 0;
            }
        };

        // Singleton access
        static FrameResourceManager& Get();

        // Initialize the manager (call after OpenGL context is ready)
        void Init();

        // Shutdown and release all resources
        void Shutdown();

        // Begin a new frame - returns frame index [0, NUM_BUFFERED_FRAMES)
        // Waits for GPU fence if necessary
        u32 BeginFrame();

        // End the current frame - inserts GPU fence
        void EndFrame();

        // Get the current frame index (thread-safe)
        // Worker threads call this to get the current frame for allocation
        u32 GetCurrentFrameIndex() const;

        // Get an allocator for the current frame
        // Thread-safe: uses atomic index to assign allocators to threads
        CommandAllocator* GetFrameAllocator();

        // Get current frame's resources directly
        FrameResources& GetCurrentFrameResources();

        // Query if double-buffering is active
        bool IsDoubleBufferingEnabled() const
        {
            return m_DoubleBufferingEnabled;
        }

        // Enable/disable double-buffering (default: enabled)
        void SetDoubleBufferingEnabled(bool enabled)
        {
            m_DoubleBufferingEnabled = enabled;
        }

        // Get total frame count since init
        u64 GetTotalFrameCount() const
        {
            return m_TotalFrameCount;
        }

        // Check if GPU has finished a specific frame
        bool IsFrameComplete(u32 frameIndex) const;

        // Wait for a specific frame to complete on GPU
        void WaitForFrame(u32 frameIndex);

      private:
        FrameResourceManager() = default;
        ~FrameResourceManager() = default;

        // Non-copyable and non-moveable
        FrameResourceManager(const FrameResourceManager&) = delete;
        FrameResourceManager& operator=(const FrameResourceManager&) = delete;
        FrameResourceManager(FrameResourceManager&&) = delete;
        FrameResourceManager& operator=(FrameResourceManager&&) = delete;

        // Create GPU sync fence
        u64 CreateFence();

        // Wait for GPU fence to be signaled
        void WaitForFence(u64 fenceId);

        // Check if fence is signaled
        bool IsFenceSignaled(u64 fenceId) const;

        // Delete a fence
        void DeleteFence(u64 fenceId);

        std::array<FrameResources, NUM_BUFFERED_FRAMES> m_FrameResources;
        // Atomic frame index: main thread writes with release, worker threads read with acquire
        // to synchronize access to m_FrameResources and frame-local allocators
        std::atomic<u32> m_CurrentFrameIndex{ 0 };
        u64 m_TotalFrameCount = 0;
        bool m_DoubleBufferingEnabled = true;
        bool m_Initialized = false;

        // Atomic allocator index for thread-safe allocator assignment
        std::atomic<u32> m_CurrentAllocatorIndex{ 0 };
    };

} // namespace OloEngine
