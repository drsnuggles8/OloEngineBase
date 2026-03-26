#pragma once

// Allpass filter for Freeverb reverb algorithm.
// Original by Jezar at Dreampoint (public domain).
// Adapted for OloEngine with modern C++ style.

#include "OloEngine/Audio/DSP/Denormals.h"
#include <vector>

namespace OloEngine::Audio::DSP
{
    class AllpassFilter
    {
      public:
        AllpassFilter() = default;

        void SetBuffer(std::vector<float>& buffer)
        {
            m_Buffer = buffer.data();
            m_BufferSize = static_cast<int>(buffer.size());
        }

        void SetFeedback(float val) { m_Feedback = val; }
        [[nodiscard]] float GetFeedback() const { return m_Feedback; }

        void Mute()
        {
            for (int i = 0; i < m_BufferSize; ++i)
            {
                m_Buffer[i] = 0.0f;
            }
        }

        inline float Process(float input);

      private:
        float m_Feedback = 0.0f;
        float* m_Buffer = nullptr;
        int m_BufferSize = 0;
        int m_BufferIndex = 0;
    };

    // Inlined for performance — called per-sample in the reverb inner loop.
    inline float AllpassFilter::Process(float input)
    {
        float bufOut = m_Buffer[m_BufferIndex];
        Undenormalize(bufOut);

        float output = -input + bufOut;
        m_Buffer[m_BufferIndex] = input + (bufOut * m_Feedback);

        if (++m_BufferIndex >= m_BufferSize)
        {
            m_BufferIndex = 0;
        }

        return output;
    }
} // namespace OloEngine::Audio::DSP
