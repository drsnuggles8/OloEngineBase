#pragma once

// Variable-length circular delay line for audio DSP.
// Supports per-channel push/pop with configurable delay in samples or milliseconds.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

namespace OloEngine::Audio::DSP
{
    class DelayLine
    {
      public:
        explicit DelayLine(int maximumDelayInSamples = 0)
        {
            assert(maximumDelayInSamples >= 0);
            m_TotalSize = std::max(4, maximumDelayInSamples + 1);
        }

        void SetDelay(float newDelayInSamples)
        {
            auto upperLimit = static_cast<float>(m_TotalSize - 1);
            m_Delay = std::clamp(newDelayInSamples, 1.0f, upperLimit);
            m_DelayInt = static_cast<int>(std::floor(m_Delay));
        }

        void SetDelayMs(u32 milliseconds)
        {
            SetDelay(static_cast<float>(static_cast<double>(milliseconds) / 1000.0 * m_SampleRate));
        }

        [[nodiscard]] float GetDelay() const
        {
            return m_Delay;
        }
        [[nodiscard]] u32 GetDelayMs() const
        {
            return static_cast<u32>(m_Delay / m_SampleRate * 1000.0);
        }
        [[nodiscard]] double GetSampleRate() const
        {
            return m_SampleRate;
        }

        void SetConfig(u32 numChannels, double sampleRate)
        {
            assert(numChannels > 0);
            m_BufferData.resize(numChannels);
            for (auto& ch : m_BufferData)
            {
                ch.assign(m_TotalSize, 0.0f);
            }
            m_WritePos.resize(numChannels);
            m_ReadPos.resize(numChannels);
            m_SampleRate = sampleRate;
            Reset();
        }

        void Reset()
        {
            std::fill(m_WritePos.begin(), m_WritePos.end(), 0);
            std::fill(m_ReadPos.begin(), m_ReadPos.end(), 0);
            for (auto& ch : m_BufferData)
            {
                std::fill(ch.begin(), ch.end(), 0.0f);
            }
        }

        void PushSample(int channel, float sample)
        {
            assert(channel >= 0 && channel < static_cast<int>(m_BufferData.size()));
            m_BufferData[channel][m_WritePos[channel]] = sample;
            m_WritePos[channel] = (m_WritePos[channel] + m_TotalSize - 1) % m_TotalSize;
        }

        float PopSample(int channel, float delayInSamples = -1.0f, bool updateReadPointer = true)
        {
            assert(channel >= 0 && channel < static_cast<int>(m_BufferData.size()));

            // Compute delay locally to avoid mutating shared m_Delay/m_DelayInt
            int delayInt = m_DelayInt;
            if (delayInSamples >= 0.0f)
            {
                auto upperLimit = static_cast<float>(m_TotalSize - 1);
                float clamped = std::clamp(delayInSamples, 1.0f, upperLimit);
                delayInt = static_cast<int>(std::floor(clamped));
            }

            int index = (m_ReadPos[channel] + delayInt) % m_TotalSize;
            float result = m_BufferData[channel][index];

            if (updateReadPointer)
            {
                m_ReadPos[channel] = (m_ReadPos[channel] + m_TotalSize - 1) % m_TotalSize;
            }

            return result;
        }

      private:
        double m_SampleRate = 44100.0;
        std::vector<std::vector<float>> m_BufferData;
        std::vector<int> m_WritePos;
        std::vector<int> m_ReadPos;
        float m_Delay = 1.0f;
        int m_DelayInt = 1;
        int m_TotalSize = 4;
    };
} // namespace OloEngine::Audio::DSP
