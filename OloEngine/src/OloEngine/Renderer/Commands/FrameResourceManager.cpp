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

        OLO_CORE_INFO("FrameResourceManager: Initializing with {} buffered frames, {} allocators per frame",
                      NUM_BUFFERED_FRAMES, ALLOCATORS_PER_FRAME);

        // Initialize frame resources for each buffer
        for (u32 frameIdx = 0; frameIdx < NUM_BUFFERED_FRAMES; ++frameIdx)
        {
            auto& frame = m_FrameResources[frameIdx];
            frame.Allocators.reserve(ALLOCATORS_PER_FRAME);

            for (u32 i = 0; i < ALLOCATORS_PER_FRAME; ++i)
            {
                frame.Allocators.push_back(std::make_unique<CommandAllocator>());
            }

            frame.FenceId = 0;
            frame.FenceSignaled = true;
            frame.AllocatorIndex = 0;
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

            frame.Allocators.clear();
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

        // When double-buffering, we need to wait for the frame we're about to reuse
        if (m_DoubleBufferingEnabled && m_TotalFrameCount >= NUM_BUFFERED_FRAMES)
        {
            // Wait for the frame that's about to be reused
            WaitForFrame(m_CurrentFrameIndex);
        }

        // Reset the current frame's resources
        auto& frame = m_FrameResources[m_CurrentFrameIndex];
        frame.Reset();
        frame.FenceSignaled = false;

        // Reset atomic allocator index for this frame
        m_CurrentAllocatorIndex.store(0, std::memory_order_relaxed);

        return m_CurrentFrameIndex;
    }

    void FrameResourceManager::EndFrame()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            OLO_CORE_ERROR("FrameResourceManager::EndFrame: Not initialized!");
            return;
        }

        auto& frame = m_FrameResources[m_CurrentFrameIndex];

        // Delete the old fence if it exists
        if (frame.FenceId != 0)
        {
            DeleteFence(frame.FenceId);
        }

        // Create a new fence for this frame's GPU work
        if (m_DoubleBufferingEnabled)
        {
            frame.FenceId = CreateFence();
        }
        else
        {
            frame.FenceSignaled = true;
        }

        // Advance to the next frame buffer
        m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % NUM_BUFFERED_FRAMES;
        m_TotalFrameCount++;
    }

    CommandAllocator* FrameResourceManager::GetFrameAllocator()
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("FrameResourceManager::GetFrameAllocator: Not initialized!");
            return nullptr;
        }

        auto& frame = m_FrameResources[m_CurrentFrameIndex];

        // Atomically get the next allocator index
        u32 index = m_CurrentAllocatorIndex.fetch_add(1, std::memory_order_relaxed);

        // Diagnostic logging for allocator index wrapping
        if (index >= ALLOCATORS_PER_FRAME)
        {
            OLO_CORE_WARN("FrameResourceManager: Allocator index wrapped ({} -> {})",
                          index, index % ALLOCATORS_PER_FRAME);
        }

        // Wrap around if we exceed the number of allocators
        index = index % ALLOCATORS_PER_FRAME;

        if (index < frame.Allocators.size())
        {
            return frame.Allocators[index].get();
        }

        OLO_CORE_ERROR("FrameResourceManager::GetFrameAllocator: No allocator available (index {})", index);
        return nullptr;
    }

    FrameResourceManager::FrameResources& FrameResourceManager::GetCurrentFrameResources()
    {
        return m_FrameResources[m_CurrentFrameIndex];
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
            WaitForFence(frame.FenceId);
        }

        frame.FenceSignaled = true;
    }

    // ========================================================================
    // OpenGL Fence Implementation
    // ========================================================================

    u64 FrameResourceManager::CreateFence()
    {
        OLO_PROFILE_FUNCTION();

        // glFenceSync returns a GLsync which we cast to u64 to preserve the full pointer value
        GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sync)
        {
            OLO_CORE_ERROR("FrameResourceManager::CreateFence: glFenceSync failed!");
            return 0;
        }

        // Store the sync object as a pointer cast to u64 for the fence ID
        // u64 preserves the full pointer value on both 32-bit and 64-bit platforms
        return static_cast<u64>(reinterpret_cast<uptr>(sync));
    }

    void FrameResourceManager::WaitForFence(u64 fenceId)
    {
        OLO_PROFILE_FUNCTION();

        if (fenceId == 0)
            return;

        GLsync sync = reinterpret_cast<GLsync>(static_cast<uptr>(fenceId));

        // Wait for the fence with a 1-second timeout
        constexpr GLuint64 TIMEOUT_NS = 1000000000ULL; // 1 second
        GLenum result = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, TIMEOUT_NS);

        if (result == GL_TIMEOUT_EXPIRED)
        {
            OLO_CORE_WARN("FrameResourceManager::WaitForFence: Fence wait timed out!");
        }
        else if (result == GL_WAIT_FAILED)
        {
            OLO_CORE_ERROR("FrameResourceManager::WaitForFence: Fence wait failed!");
        }
        // GL_ALREADY_SIGNALED or GL_CONDITION_SATISFIED means success
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
