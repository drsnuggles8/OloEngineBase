#pragma once

// Freeverb reverb model — stereo reverb using 8 parallel comb filters
// feeding 4 series allpass filters per channel.
// Original algorithm by Jezar at Dreampoint (public domain).
// Adapted for OloEngine with modern C++, std::vector buffers, and
// sample-rate-independent buffer sizing.

#include "OloEngine/Audio/DSP/AllpassFilter.h"
#include "OloEngine/Audio/DSP/CombFilter.h"
#include "OloEngine/Audio/DSP/Tuning.h"
#include <atomic>
#include <vector>

namespace OloEngine::Audio::DSP
{
    class ReverbModel
    {
      public:
        explicit ReverbModel(double sampleRate);

        void Mute();

        // Process numSamples frames, replacing output
        void ProcessReplace(const float* inputL, const float* inputR, float* outputL, float* outputR, i64 numSamples, int skip);

        void SetRoomSize(float value);
        [[nodiscard]] float GetRoomSize() const;
        void SetDamp(float value);
        [[nodiscard]] float GetDamp() const;
        void SetWet(float value);
        [[nodiscard]] float GetWet() const;
        void SetDry(float value);
        [[nodiscard]] float GetDry() const;
        void SetWidth(float value);
        [[nodiscard]] float GetWidth() const;
        void SetMode(float value);
        [[nodiscard]] float GetMode() const;

      private:
        void Update();

        // Thread-safe parameter block: Update() writes to inactive slot, then swaps.
        // ProcessReplace() reads from active slot without locks.
        struct ParamBlock
        {
            float Gain = 0.0f;
            float RoomSize1 = 0.0f;
            float Damp1 = 0.0f;
            float Wet1 = 0.0f;
            float Wet2 = 0.0f;
            float Dry = 0.0f;
        };

        ParamBlock m_Params[2]{};
        std::atomic<int> m_ActiveParamIndex{ 0 };

        // Authoring-side state (only modified by game thread via setters)
        float m_RoomSize = 0.0f;
        float m_Damp = 0.0f;
        float m_Wet = 0.0f;
        float m_Dry = 0.0f;
        float m_Width = 0.0f;
        float m_Mode = 0.0f;

        CombFilter m_CombL[NumCombs];
        CombFilter m_CombR[NumCombs];
        AllpassFilter m_AllpassL[NumAllpasses];
        AllpassFilter m_AllpassR[NumAllpasses];

        // Comb filter buffers
        std::vector<float> m_BufCombL1, m_BufCombR1;
        std::vector<float> m_BufCombL2, m_BufCombR2;
        std::vector<float> m_BufCombL3, m_BufCombR3;
        std::vector<float> m_BufCombL4, m_BufCombR4;
        std::vector<float> m_BufCombL5, m_BufCombR5;
        std::vector<float> m_BufCombL6, m_BufCombR6;
        std::vector<float> m_BufCombL7, m_BufCombR7;
        std::vector<float> m_BufCombL8, m_BufCombR8;

        // Allpass filter buffers
        std::vector<float> m_BufAllpassL1, m_BufAllpassR1;
        std::vector<float> m_BufAllpassL2, m_BufAllpassR2;
        std::vector<float> m_BufAllpassL3, m_BufAllpassR3;
        std::vector<float> m_BufAllpassL4, m_BufAllpassR4;
    };
} // namespace OloEngine::Audio::DSP
