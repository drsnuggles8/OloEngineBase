#include "OloEnginePCH.h"
#include "InflightFrameManager.h"
#include "OloEngine/Core/Log.h"
#include <thread>

namespace OloEngine
{
    void InflightFrameManager::BeginFrame()
    {
        // Mark previous frame as complete
        if (m_CompletedFrames > 0)
        {
            u32 prevFrame = (m_CurrentFrameIndex + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
            m_Frames[prevFrame].IsComplete = true;
        }

        // Move to next frame
        m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
        
        // Reset current frame data
        auto& currentFrame = m_Frames[m_CurrentFrameIndex];
        currentFrame.IsComplete = false;
        currentFrame.StartTime = std::chrono::steady_clock::now();
        currentFrame.BufferAllocations.clear();
        
        m_CompletedFrames++;
    }

    void InflightFrameManager::EndFrame()
    {
        auto& currentFrame = m_Frames[m_CurrentFrameIndex];
        // Frame will be marked complete at the beginning of next frame
        // This gives GPU time to process the commands
    }

    Ref<UniformBuffer> InflightFrameManager::GetFrameUniformBuffer(const std::string& name, u32 size)
    {
        auto& currentFrame = m_Frames[m_CurrentFrameIndex];
        
        // Check if we already have a buffer allocation for this name
        auto it = currentFrame.BufferAllocations.find(name);
        if (it != currentFrame.BufferAllocations.end())
        {
            u32 bufferIndex = it->second;
            if (bufferIndex < currentFrame.UniformBuffers.size())
            {
                auto& buffer = currentFrame.UniformBuffers[bufferIndex];
                if (buffer && buffer->GetSize() >= size)
                {
                    return buffer;
                }
            }
        }
        
        // Create new buffer for this frame
        auto buffer = UniformBuffer::Create(size, 0); // Use binding 0 as default
        u32 bufferIndex = static_cast<u32>(currentFrame.UniformBuffers.size());
        currentFrame.UniformBuffers.push_back(buffer);
        currentFrame.BufferAllocations[name] = bufferIndex;
        
        OLO_CORE_TRACE("InflightFrameManager: Created buffer '{0}' for frame {1}, size {2}", 
                      name, m_CurrentFrameIndex, size);
        
        return buffer;
    }

    void InflightFrameManager::ReleaseFrameBuffers(u32 frameIndex)
    {
        if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
            return;
            
        auto& frame = m_Frames[frameIndex];
        frame.UniformBuffers.clear();
        frame.BufferAllocations.clear();
        frame.IsComplete = false;
    }

    void InflightFrameManager::WaitForFrame(u32 frameIndex)
    {
        if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
            return;
            
        // Simple implementation - in a real engine you'd use GPU fences
        auto& frame = m_Frames[frameIndex];
        while (!frame.IsComplete)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    bool InflightFrameManager::IsFrameComplete(u32 frameIndex) const
    {
        if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
            return true;
            
        return m_Frames[frameIndex].IsComplete;
    }
}
