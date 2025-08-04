#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include <array>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace OloEngine
{
    /**
     * @brief Manages multiple frames in flight to prevent GPU stalls
     */
    class InflightFrameManager : public RefCounted
    {
    public:
        static constexpr u32 MAX_FRAMES_IN_FLIGHT = 3;
        
        /**
         * @brief Data for a single frame in flight
         */
        struct FrameData
        {
            std::vector<AssetRef<UniformBuffer>> UniformBuffers;
            std::unordered_map<std::string, u32> BufferAllocations;
            bool IsComplete = false;
            std::chrono::steady_clock::time_point StartTime;
        };

        InflightFrameManager() = default;
        ~InflightFrameManager() = default;

        /**
         * @brief Begin a new frame
         */
        void BeginFrame();

        /**
         * @brief End the current frame
         */
        void EndFrame();

        /**
         * @brief Get current frame index
         */
        u32 GetCurrentFrameIndex() const { return m_CurrentFrameIndex; }

        /**
         * @brief Get uniform buffer for current frame
         */
        AssetRef<UniformBuffer> GetFrameUniformBuffer(const std::string& name, u32 size);

        /**
         * @brief Release buffers for a specific frame
         */
        void ReleaseFrameBuffers(u32 frameIndex);

        /**
         * @brief Wait for frame to complete (basic implementation)
         */
        void WaitForFrame(u32 frameIndex);

        /**
         * @brief Check if frame is complete
         */
        bool IsFrameComplete(u32 frameIndex) const;

    private:
        std::array<FrameData, MAX_FRAMES_IN_FLIGHT> m_Frames;
        u32 m_CurrentFrameIndex = 0;
        u32 m_CompletedFrames = 0;
    };
}
