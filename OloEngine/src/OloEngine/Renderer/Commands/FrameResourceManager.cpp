#include "OloEnginePCH.h"
#include "FrameResourceManager.h"

#include <glad/gl.h>

namespace OloEngine
{
    FrameResourceManager& FrameResourceManager::Get()
    {
        static FrameResourceManager instance;
        return instance;
    }

    void FrameResourceManager::Init()
    {
        OLO_PROFILE_FUNCTION();

        if (m_Initialized)
        {
            OLO_CORE_WARN("FrameResourceManager::Init: Already initialized");
            return;
        }

        OLO_CORE_INFO("FrameResourceManager: Initializing with {} buffered frames, 1 main + {} worker allocators per frame",
                      NUM_BUFFERED_FRAMES, MAX_WORKERS);

        // FrameResources are default-constructed (CommandAllocator uses DEFAULT_BLOCK_SIZE).
        // Just reset fence state.
        for (u32 frameIdx = 0; frameIdx < NUM_BUFFERED_FRAMES; ++frameIdx)
        {
            auto& frame = m_FrameResources[frameIdx];
            frame.FenceId = 0;
            frame.FenceSignaled = true;
        }

        m_CurrentFrameIndex = 0;
        m_TotalFrameCount = 0;
        m_Initialized = true;

        OLO_CORE_INFO("FrameResourceManager: Initialized successfully");
    }

    void FrameResourceManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
            return;

        OLO_CORE_INFO("FrameResourceManager: Shutting down...");

        // Wait for all frames to complete on GPU
        for (u32 frameIdx = 0; frameIdx < NUM_BUFFERED_FRAMES; ++frameIdx)
        {
            WaitForFrame(frameIdx);

            auto& frame = m_FrameResources[frameIdx];
            if (frame.FenceId != 0)
            {
                DeleteFence(frame.FenceId);
                frame.FenceId = 0;
            }
        }

        m_Initialized = false;
        OLO_CORE_INFO("FrameResourceManager: Shutdown complete");
    }

    u32 FrameResourceManager::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            OLO_CORE_ERROR("FrameResourceManager::BeginFrame: Not initialized!");
            return 0;
        }

        u32 currentIndex = m_CurrentFrameIndex.load(std::memory_order_acquire);

        // When double-buffering, we need to wait for the frame we're about to reuse
        if (m_DoubleBufferingEnabled && m_TotalFrameCount >= NUM_BUFFERED_FRAMES)
        {
            WaitForFrame(currentIndex);

            // Only reset allocators if the GPU fence was actually signaled;
            // otherwise the GPU may still be reading this frame's data.
            if (!m_FrameResources[currentIndex].FenceSignaled)
            {
                OLO_CORE_ERROR("FrameResourceManager::BeginFrame: Fence wait failed for frame {}, skipping allocator reset", currentIndex);
                return currentIndex;
            }
        }

        // Reset ALL allocators (main + workers) for this frame
        m_FrameResources[currentIndex].Reset();
        m_FrameResources[currentIndex].FenceSignaled = false;

        return currentIndex;
    }

    void FrameResourceManager::EndFrame()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            OLO_CORE_ERROR("FrameResourceManager::EndFrame: Not initialized!");
            return;
        }

        u32 currentIndex = m_CurrentFrameIndex.load(std::memory_order_relaxed);
        auto& frame = m_FrameResources[currentIndex];

        // Delete the old fence if it exists
        if (frame.FenceId != 0)
        {
            DeleteFence(frame.FenceId);
        }

        // Create a new fence for this frame's GPU work
        if (m_DoubleBufferingEnabled)
        {
            frame.FenceId = CreateFence();
            if (frame.FenceId == 0)
            {
                OLO_CORE_ERROR("FrameResourceManager::EndFrame: Failed to create GPU fence!");
                frame.FenceSignaled = true;
            }
        }
        else
        {
            frame.FenceSignaled = true;
        }

        // Advance to the next frame buffer
        u32 nextIndex = (currentIndex + 1) % NUM_BUFFERED_FRAMES;
        m_CurrentFrameIndex.store(nextIndex, std::memory_order_release);
        m_TotalFrameCount++;
    }

    CommandAllocator* FrameResourceManager::GetMainAllocator()
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("FrameResourceManager::GetMainAllocator: Not initialized!");
            return nullptr;
        }

        u32 currentIndex = m_CurrentFrameIndex.load(std::memory_order_acquire);
        return &m_FrameResources[currentIndex].MainAllocator;
    }

    CommandAllocator* FrameResourceManager::GetWorkerAllocator(u32 workerIndex)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("FrameResourceManager::GetWorkerAllocator: Not initialized!");
            return nullptr;
        }

        if (workerIndex >= MAX_WORKERS)
        {
            OLO_CORE_ERROR("FrameResourceManager::GetWorkerAllocator: Worker index {} exceeds max {}!",
                           workerIndex, MAX_WORKERS);
            return nullptr;
        }

        u32 currentIndex = m_CurrentFrameIndex.load(std::memory_order_acquire);
        if (currentIndex >= NUM_BUFFERED_FRAMES)
        {
            OLO_CORE_ERROR("FrameResourceManager::GetWorkerAllocator: Invalid frame index {}!", currentIndex);
            return nullptr;
        }

        return &m_FrameResources[currentIndex].WorkerAllocators[workerIndex];
    }

    bool FrameResourceManager::IsFrameComplete(u32 frameIndex) const
    {
        if (frameIndex >= NUM_BUFFERED_FRAMES)
            return true;

        const auto& frame = m_FrameResources[frameIndex];
        if (frame.FenceSignaled)
            return true;

        if (frame.FenceId == 0)
            return true;

        return IsFenceSignaled(frame.FenceId);
    }

    void FrameResourceManager::WaitForFrame(u32 frameIndex)
    {
        OLO_PROFILE_FUNCTION();

        if (frameIndex >= NUM_BUFFERED_FRAMES)
            return;

        auto& frame = m_FrameResources[frameIndex];

        if (frame.FenceSignaled)
            return;

        if (frame.FenceId != 0)
        {
            if (WaitForFence(frame.FenceId))
            {
                frame.FenceSignaled = true;
            }
            else
            {
                OLO_CORE_WARN("FrameResourceManager::WaitForFrame: Fence wait did not succeed for frame {}, skipping reset", frameIndex);
            }
        }
        else
        {
            frame.FenceSignaled = true;
        }
    }

    u32 FrameResourceManager::GetCurrentFrameIndex() const
    {
        return m_CurrentFrameIndex.load(std::memory_order_acquire);
    }

    // ========================================================================
    // OpenGL Fence Implementation
    // ========================================================================

    u64 FrameResourceManager::CreateFence()
    {
        OLO_PROFILE_FUNCTION();

        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sync)
        {
            OLO_CORE_ERROR("FrameResourceManager::CreateFence: glFenceSync failed!");
            return 0;
        }

        return static_cast<u64>(reinterpret_cast<uptr>(sync));
    }

    bool FrameResourceManager::WaitForFence(u64 fenceId)
    {
        OLO_PROFILE_FUNCTION();

        if (fenceId == 0)
            return true;

        GLsync sync = reinterpret_cast<GLsync>(static_cast<uptr>(fenceId));

        constexpr GLuint64 TIMEOUT_NS = 1000000000ULL; // 1 second
        GLenum result = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, TIMEOUT_NS);

        if (result == GL_TIMEOUT_EXPIRED)
        {
            OLO_CORE_WARN("FrameResourceManager::WaitForFence: Fence wait timed out!");
            return false;
        }
        else if (result == GL_WAIT_FAILED)
        {
            OLO_CORE_ERROR("FrameResourceManager::WaitForFence: Fence wait failed!");
            return false;
        }

        return true;
    }

    bool FrameResourceManager::IsFenceSignaled(u64 fenceId) const
    {
        if (fenceId == 0)
            return true;

        GLsync sync = reinterpret_cast<GLsync>(static_cast<uptr>(fenceId));

        GLint signaled = GL_FALSE;
        GLsizei length = 0;
        glGetSynciv(sync, GL_SYNC_STATUS, sizeof(signaled), &length, &signaled);

        return signaled == GL_SIGNALED;
    }

    void FrameResourceManager::DeleteFence(u64 fenceId)
    {
        if (fenceId == 0)
            return;

        GLsync sync = reinterpret_cast<GLsync>(static_cast<uptr>(fenceId));
        glDeleteSync(sync);
    }

} // namespace OloEngine
