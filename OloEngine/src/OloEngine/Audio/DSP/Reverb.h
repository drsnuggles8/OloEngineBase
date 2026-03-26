#pragma once

// Reverb DSP effect — Freeverb-based stereo reverb with configurable pre-delay.
// Wraps ReverbModel as a miniaudio node that can be attached to the audio graph.

#include <memory>

struct ma_engine;
struct ma_node_base;

namespace OloEngine::Audio::DSP
{
    class DelayLine;
    class ReverbModel;

    enum class ReverbParameter : u8
    {
        PreDelay = 0,
        Mode,
        RoomSize,
        Damp,
        Width,
        Wet,
        Dry,
        Count
    };

    class Reverb
    {
      public:
        Reverb();
        ~Reverb();

        Reverb(const Reverb&) = delete;
        Reverb& operator=(const Reverb&) = delete;

        bool Initialize(ma_engine* engine, ma_node_base* nodeToAttachTo);
        void Uninitialize();
        [[nodiscard]] ma_node_base* GetNode();

        void SetParameter(ReverbParameter parameter, float value);
        [[nodiscard]] float GetParameter(ReverbParameter parameter) const;

      private:
        friend void ReverbNodeProcessPcmFrames(void* pNode, const float** ppFramesIn, unsigned int* pFrameCountIn,
                                               float** ppFramesOut, unsigned int* pFrameCountOut);

        struct ReverbNode; // Defined in .cpp — embeds ma_node_base by value

        bool m_Initialized = false;
        float m_MaxPreDelay = 1000.0f;
        ReverbNode* m_Node = nullptr;
        std::unique_ptr<ReverbModel> m_RevModel;
        std::unique_ptr<DelayLine> m_DelayLine;
    };
} // namespace OloEngine::Audio::DSP
