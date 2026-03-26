#pragma once

// VBAP (Vector Base Amplitude Panning) — 2D panning algorithm for multichannel
// audio spatialization. Ported from Hazel Engine's VBAP implementation.
//
// Calculates per-channel gain coefficients based on source direction relative
// to a speaker layout. Supports stereo through surround configurations via
// virtual source distribution and inverse-matrix speaker pair lookup.

#include "OloEngine/Core/Base.h"

#include <array>
#include <atomic>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/mat2x2.hpp>

#include <miniaudio.h>

namespace OloEngine::Audio::DSP
{
    using ChannelGains = std::array<float, MA_MAX_CHANNELS>;

    // Double-buffered atomic gain container for thread-safe gain updates.
    // Non-realtime thread writes (flipping), realtime thread reads (lockfree).
    class RealtimeGains
    {
      public:
        RealtimeGains()
        {
            m_Buffers[0].fill(0.0f);
            m_Buffers[1].fill(0.0f);
        }

        RealtimeGains(const RealtimeGains&) = delete;
        RealtimeGains& operator=(const RealtimeGains&) = delete;

        RealtimeGains(RealtimeGains&& other) noexcept
            : m_ActiveIndex(other.m_ActiveIndex.load(std::memory_order_acquire))
        {
            m_Buffers[0] = other.m_Buffers[0];
            m_Buffers[1] = other.m_Buffers[1];
        }

        RealtimeGains& operator=(RealtimeGains&& other) noexcept
        {
            if (this != &other)
            {
                m_Buffers[0] = other.m_Buffers[0];
                m_Buffers[1] = other.m_Buffers[1];
                m_ActiveIndex.store(other.m_ActiveIndex.load(std::memory_order_acquire), std::memory_order_release);
            }
            return *this;
        }

        // Write new gains from the non-realtime thread and publish atomically
        void Write(const ChannelGains& gains)
        {
            u32 writeIdx = 1 - m_ActiveIndex.load(std::memory_order_acquire);
            m_Buffers[writeIdx] = gains;
            m_ActiveIndex.store(writeIdx, std::memory_order_release);
        }

        // Read the latest gains from the realtime thread (lockfree)
        [[nodiscard]] const ChannelGains& Read() const
        {
            return m_Buffers[m_ActiveIndex.load(std::memory_order_acquire)];
        }

      private:
        ChannelGains m_Buffers[2]{};
        std::atomic<u32> m_ActiveIndex{ 0 };
    };

    struct VBAPData;

    class VBAP
    {
      public:
        struct PositionUpdateData
        {
            float PanAngle;
            float Spread;
            float Focus;
            float GainAttenuation;
        };

        struct VirtualSource
        {
            float Angle = 0.0f;
            u32 Channel = 0;
            u32 NumOutputChannels = 0;
            ChannelGains Gains{};

            VirtualSource() = default;
            VirtualSource(u32 numberOfOutputChannels, float angle = 0.0f);
        };

        struct ChannelGroup
        {
            float Angle = 0.0f;
            u32 Channel = 0;
            std::vector<i32> VirtualSourceIDs;

            RealtimeGains Gains;
            ma_gainer Gainer{};

            ChannelGroup();
            ChannelGroup(u32 numberOfOutputChannels, u32 channel, float angle = 0.0f);
            ChannelGroup(const ChannelGroup&) = delete;
            ChannelGroup& operator=(const ChannelGroup&) = delete;
            ChannelGroup(ChannelGroup&&) = default;
            ChannelGroup& operator=(ChannelGroup&&) = default;
        };

      public:
        static bool InitVBAP(VBAPData* vbap, u32 numOfInputs, u32 numOfOutputs, const ma_channel* sourceChannelMap,
                             const ma_channel* outputChannelMap);
        static void ClearVBAP(VBAPData* vbap);
        static void UpdateVBAP(VBAPData* vbap, const PositionUpdateData& positionData,
                               const ma_channel_converter& converter, bool isInitialPosition = false);

      private:
        static void SortChannelLayout(const std::vector<glm::vec2>& speakerVectors,
                                      std::vector<std::pair<glm::vec2, u32>>& sorted);
        static void ProcessVBAP(const VBAPData* vbap, float azimuthRadians, ChannelGains& gains);
        static bool FindActiveArch(const VBAPData* vbap, float azimuthRadians, std::pair<i32, i32>& outSpeakerIndexes,
                                   std::pair<float, float>& outGains);
        static ChannelGains ConvertChannelGains(const ChannelGains& channelGainsIn,
                                                const ma_channel_converter& converter);
    };

    struct VBAPData
    {
        std::vector<glm::vec2> spPos;
        std::vector<std::pair<glm::vec2, u32>> spPosSorted;
        std::vector<glm::mat2> InverseMats;
        std::vector<VBAP::VirtualSource> VirtualSources;
        std::vector<VBAP::ChannelGroup> ChannelGroups;
    };

    // Calculate angle in XZ where -Z is forward direction
    inline float VectorAngle(const glm::vec3& vec)
    {
        glm::vec3 norm = glm::normalize(vec);
        float x = norm.x;
        float y = norm.z;

        if (x == 0.0f)
        {
            return glm::radians((y < 0.0f) ? 0.0f : (y == 0.0f) ? 0.0f
                                                                : 180.0f);
        }
        if (y == 0.0f)
        {
            return glm::radians((x > 0.0f) ? 90.0f : (x < 0.0f) ? -90.0f
                                                                : 0.0f);
        }

        float ret = glm::degrees(atanf(x / y));

        if (x < 0.0f && y < 0.0f)
            ret *= -1.0f;
        else if (x < 0.0f)
            ret = (180.0f + ret) * -1.0f;
        else if (y < 0)
            ret *= -1.0f;
        else
            ret = 180.0f - ret;

        return glm::radians(ret);
    }

    inline float VectorAngle(const glm::vec2& vec)
    {
        glm::vec2 norm = glm::normalize(vec);
        float x = norm.x;
        float y = norm.y;

        if (x == 0.0f)
        {
            return glm::radians((y < 0.0f) ? 0.0f : (y == 0.0f) ? 0.0f
                                                                : 180.0f);
        }
        if (y == 0.0f)
        {
            return glm::radians((x > 0.0f) ? 90.0f : (x < 0.0f) ? -90.0f
                                                                : 0.0f);
        }

        float ret = glm::degrees(atanf(x / y));

        if (x < 0.0f && y < 0.0f)
            ret *= -1.0f;
        else if (x < 0.0f)
            ret = (180.0f + ret) * -1.0f;
        else if (y < 0)
            ret *= -1.0f;
        else
            ret = 180.0f - ret;

        return glm::radians(ret);
    }

} // namespace OloEngine::Audio::DSP
