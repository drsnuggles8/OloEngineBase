#pragma once

#include "OloEngine/Core/Base.h"
#include "CommandAllocator.h"
#include "CommandBucket.h"
#include <array>
#include <atomic>
#include <memory>

namespace OloEngine
{
    /**
     * @brief Double-buffered frame resource management with per-worker allocators.
     *
     * Manages two sets of command allocators (one main-thread + MAX_WORKERS per frame),
     * allowing the CPU to build frame N+1 commands while the GPU executes frame N.
     * GPU fence synchronization prevents reusing memory before the GPU is done.
     *
     * This class replaces the old FrameResourceManager + CommandMemoryManager split,
     * following Molecular Matters' design: each thread gets its own allocator with
     * zero synchronization on the allocation hot path.
     *
     * Usage:
     *   1. BeginFrame() — waits for GPU fence, resets current frame's allocators
     *   2. GetMainAllocator() — main-thread allocator for command recording
     *   3. GetWorkerAllocator(i) — per-worker allocator for parallel submission
     *   4. EndFrame() — inserts GPU fence, advances frame index
     *
     * Thread Safety:
     *   - BeginFrame()/EndFrame() must be called from the main thread only
     *   - Each allocator must only be used by its owning thread
     */
    class FrameResourceManager
    {
      public:
        static constexpr u32 NUM_BUFFERED_FRAMES = 2;
        static constexpr u32 MAX_WORKERS = 16;

        struct FrameResources
        {
            CommandAllocator MainAllocator;
            std::array<CommandAllocator, MAX_WORKERS> WorkerAllocators;

            // GPU fence for synchronization
            u64 FenceId = 0;
            bool FenceSignaled = true;

            void Reset()
            {
                MainAllocator.Reset();
                for (auto& alloc : WorkerAllocators)
                    alloc.Reset();
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
        u32 GetCurrentFrameIndex() const;

        // Get the main-thread allocator for the current frame
        CommandAllocator* GetMainAllocator();

        // Get a per-worker allocator for the current frame
        // @param workerIndex The worker index [0, MAX_WORKERS)
        CommandAllocator* GetWorkerAllocator(u32 workerIndex);

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
        std::atomic<u32> m_CurrentFrameIndex{ 0 };
        u64 m_TotalFrameCount = 0;
        bool m_DoubleBufferingEnabled = true;
        bool m_Initialized = false;
    };

} // namespace OloEngine
