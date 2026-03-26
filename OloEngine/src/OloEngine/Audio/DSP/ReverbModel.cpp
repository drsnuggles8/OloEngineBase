#include "OloEnginePCH.h"
#include "OloEngine/Audio/DSP/ReverbModel.h"

namespace OloEngine::Audio::DSP
{
    ReverbModel::ReverbModel(double sampleRate)
    {
        OLO_CORE_ASSERT(sampleRate > 0.0, "ReverbModel: sampleRate must be positive, got {}", sampleRate);

        // Scale buffer sizes from 44100 Hz reference to the actual sample rate
        const double srCoef = 44100.0 / sampleRate;

        auto scaledSize = [&](int baseSamples) -> int
        {
            return std::max(1, static_cast<int>(static_cast<double>(baseSamples) / srCoef));
        };

        // Resize comb buffers
        m_BufCombL1.resize(scaledSize(CombTuningL1), 0.0f);
        m_BufCombR1.resize(scaledSize(CombTuningR1), 0.0f);
        m_BufCombL2.resize(scaledSize(CombTuningL2), 0.0f);
        m_BufCombR2.resize(scaledSize(CombTuningR2), 0.0f);
        m_BufCombL3.resize(scaledSize(CombTuningL3), 0.0f);
        m_BufCombR3.resize(scaledSize(CombTuningR3), 0.0f);
        m_BufCombL4.resize(scaledSize(CombTuningL4), 0.0f);
        m_BufCombR4.resize(scaledSize(CombTuningR4), 0.0f);
        m_BufCombL5.resize(scaledSize(CombTuningL5), 0.0f);
        m_BufCombR5.resize(scaledSize(CombTuningR5), 0.0f);
        m_BufCombL6.resize(scaledSize(CombTuningL6), 0.0f);
        m_BufCombR6.resize(scaledSize(CombTuningR6), 0.0f);
        m_BufCombL7.resize(scaledSize(CombTuningL7), 0.0f);
        m_BufCombR7.resize(scaledSize(CombTuningR7), 0.0f);
        m_BufCombL8.resize(scaledSize(CombTuningL8), 0.0f);
        m_BufCombR8.resize(scaledSize(CombTuningR8), 0.0f);

        // Resize allpass buffers
        m_BufAllpassL1.resize(scaledSize(AllpassTuningL1), 0.0f);
        m_BufAllpassR1.resize(scaledSize(AllpassTuningR1), 0.0f);
        m_BufAllpassL2.resize(scaledSize(AllpassTuningL2), 0.0f);
        m_BufAllpassR2.resize(scaledSize(AllpassTuningR2), 0.0f);
        m_BufAllpassL3.resize(scaledSize(AllpassTuningL3), 0.0f);
        m_BufAllpassR3.resize(scaledSize(AllpassTuningR3), 0.0f);
        m_BufAllpassL4.resize(scaledSize(AllpassTuningL4), 0.0f);
        m_BufAllpassR4.resize(scaledSize(AllpassTuningR4), 0.0f);

        // Tie components to buffers
        m_CombL[0].SetBuffer(m_BufCombL1);
        m_CombR[0].SetBuffer(m_BufCombR1);
        m_CombL[1].SetBuffer(m_BufCombL2);
        m_CombR[1].SetBuffer(m_BufCombR2);
        m_CombL[2].SetBuffer(m_BufCombL3);
        m_CombR[2].SetBuffer(m_BufCombR3);
        m_CombL[3].SetBuffer(m_BufCombL4);
        m_CombR[3].SetBuffer(m_BufCombR4);
        m_CombL[4].SetBuffer(m_BufCombL5);
        m_CombR[4].SetBuffer(m_BufCombR5);
        m_CombL[5].SetBuffer(m_BufCombL6);
        m_CombR[5].SetBuffer(m_BufCombR6);
        m_CombL[6].SetBuffer(m_BufCombL7);
        m_CombR[6].SetBuffer(m_BufCombR7);
        m_CombL[7].SetBuffer(m_BufCombL8);
        m_CombR[7].SetBuffer(m_BufCombR8);

        m_AllpassL[0].SetBuffer(m_BufAllpassL1);
        m_AllpassR[0].SetBuffer(m_BufAllpassR1);
        m_AllpassL[1].SetBuffer(m_BufAllpassL2);
        m_AllpassR[1].SetBuffer(m_BufAllpassR2);
        m_AllpassL[2].SetBuffer(m_BufAllpassL3);
        m_AllpassR[2].SetBuffer(m_BufAllpassR3);
        m_AllpassL[3].SetBuffer(m_BufAllpassL4);
        m_AllpassR[3].SetBuffer(m_BufAllpassR4);

        // Set default allpass feedback
        for (int i = 0; i < NumAllpasses; ++i)
        {
            m_AllpassL[i].SetFeedback(0.5f);
            m_AllpassR[i].SetFeedback(0.5f);
        }

        SetWet(InitialWet);
        SetRoomSize(InitialRoom);
        SetDry(InitialDry);
        SetDamp(InitialDamp);
        SetWidth(InitialWidth);
        SetMode(InitialMode);

        Mute();
    }

    void ReverbModel::Mute()
    {
        if (GetMode() >= FreezeMode)
        {
            return;
        }

        for (int i = 0; i < NumCombs; ++i)
        {
            m_CombL[i].Mute();
            m_CombR[i].Mute();
        }
        for (int i = 0; i < NumAllpasses; ++i)
        {
            m_AllpassL[i].Mute();
            m_AllpassR[i].Mute();
        }
    }

    void ReverbModel::ProcessReplace(const float* inputL, const float* inputR, float* outputL, float* outputR, i64 numSamples, int skip)
    {
        // Snapshot parameters once per buffer from the active (published) slot
        const auto& p = m_Params[m_ActiveParamIndex.load(std::memory_order_acquire)];
        const float gain = p.Gain;
        const float wet1 = p.Wet1;
        const float wet2 = p.Wet2;
        const float dry = p.Dry;

        float outL;
        float outR;
        float input;

        while (numSamples-- > 0)
        {
            outL = outR = 0.0f;
            input = (*inputL + *inputR) * gain;

            // Accumulate comb filters in parallel
            for (int i = 0; i < NumCombs; ++i)
            {
                outL += m_CombL[i].Process(input);
                outR += m_CombR[i].Process(input);
            }

            // Feed through allpasses in series
            for (int i = 0; i < NumAllpasses; ++i)
            {
                outL = m_AllpassL[i].Process(outL);
                outR = m_AllpassR[i].Process(outR);
            }

            // Replace output
            *outputL = outL * wet1 + outR * wet2 + *inputL * dry;
            *outputR = outR * wet1 + outL * wet2 + *inputR * dry;

            inputL += skip;
            inputR += skip;
            outputL += skip;
            outputR += skip;
        }
    }

    void ReverbModel::Update()
    {
        // Compute derived parameters
        float wet1 = m_Wet * (m_Width / 2.0f + 0.5f);
        float wet2 = m_Wet * ((1.0f - m_Width) / 2.0f);

        float roomSize1;
        float damp1;
        float gain;

        if (m_Mode >= FreezeMode)
        {
            roomSize1 = 1.0f;
            damp1 = 0.0f;
            gain = Muted;
        }
        else
        {
            roomSize1 = m_RoomSize;
            damp1 = m_Damp;
            gain = FixedGain;
        }

        // Write to inactive param slot, then publish atomically
        int writeIdx = 1 - m_ActiveParamIndex.load(std::memory_order_relaxed);
        m_Params[writeIdx] = { gain, roomSize1, damp1, wet1, wet2, m_Dry };
        m_ActiveParamIndex.store(writeIdx, std::memory_order_release);

        // Update per-filter coefficients (CombFilter::SetFeedback/SetDamp are simple float assignments)
        for (int i = 0; i < NumCombs; ++i)
        {
            m_CombL[i].SetFeedback(roomSize1);
            m_CombR[i].SetFeedback(roomSize1);
            m_CombL[i].SetDamp(damp1);
            m_CombR[i].SetDamp(damp1);
        }
    }

    void ReverbModel::SetRoomSize(float value)
    {
        m_RoomSize = (value * ScaleRoom) + OffsetRoom;
        Update();
    }

    float ReverbModel::GetRoomSize() const
    {
        return (m_RoomSize - OffsetRoom) / ScaleRoom;
    }

    void ReverbModel::SetDamp(float value)
    {
        m_Damp = value * ScaleDamp;
        Update();
    }

    float ReverbModel::GetDamp() const
    {
        return m_Damp / ScaleDamp;
    }

    void ReverbModel::SetWet(float value)
    {
        m_Wet = value * ScaleWet;
        Update();
    }

    float ReverbModel::GetWet() const
    {
        return m_Wet / ScaleWet;
    }

    void ReverbModel::SetDry(float value)
    {
        m_Dry = value * ScaleDry;
        Update();
    }

    float ReverbModel::GetDry() const
    {
        return m_Dry / ScaleDry;
    }

    void ReverbModel::SetWidth(float value)
    {
        m_Width = value;
        Update();
    }

    float ReverbModel::GetWidth() const
    {
        return m_Width;
    }

    void ReverbModel::SetMode(float value)
    {
        m_Mode = value;
        Update();
    }

    float ReverbModel::GetMode() const
    {
        return m_Mode;
    }
} // namespace OloEngine::Audio::DSP
