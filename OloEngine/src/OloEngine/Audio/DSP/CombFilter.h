#pragma once

// Comb filter for Freeverb reverb algorithm.
// Original by Jezar at Dreampoint (public domain).
// Adapted for OloEngine with modern C++ style.

#include "OloEngine/Audio/DSP/Denormals.h"
#include <algorithm>
#include <cassert>
#include <vector>

namespace OloEngine::Audio::DSP
{
    class CombFilter
    {
      public:
        CombFilter() = default;

        // Non-owning: caller must keep `buffer` alive for the lifetime of this filter.
        void SetBuffer(std::vector<float>& buffer)
        {
            m_Buffer = buffer.data();
            m_BufferSize = static_cast<int>(buffer.size());
        }

        void SetDamp(float val)
        {
            m_Damp1 = val;
            m_Damp2 = 1.0f - val;
        }
        [[nodiscard]] float GetDamp() const
        {
            return m_Damp1;
        }

        void SetFeedback(float val)
        {
            m_Feedback = val;
        }
        [[nodiscard]] float GetFeedback() const
        {
            return m_Feedback;
        }

        void Mute()
        {
            if (m_Buffer)
            {
                std::fill(m_Buffer, m_Buffer + m_BufferSize, 0.0f);
            }
            m_FilterStore = 0.0f;
            m_BufferIndex = 0;
        }

        inline float Process(float input);

      private:
        float m_Feedback = 0.0f;
        float m_FilterStore = 0.0f;
        float m_Damp1 = 0.0f;
        float m_Damp2 = 0.0f;
        float* m_Buffer = nullptr;
        int m_BufferSize = 0;
        int m_BufferIndex = 0;
    };

    // Inlined for performance — called per-sample in the reverb inner loop.
    inline float CombFilter::Process(float input)
    {
        assert(m_Buffer);
        float output = m_Buffer[m_BufferIndex];
        Undenormalize(output);

        m_FilterStore = (output * m_Damp2) + (m_FilterStore * m_Damp1);
        Undenormalize(m_FilterStore);

        m_Buffer[m_BufferIndex] = input + (m_FilterStore * m_Feedback);

        if (++m_BufferIndex >= m_BufferSize)
        {
            m_BufferIndex = 0;
        }

        return output;
    }
} // namespace OloEngine::Audio::DSP
