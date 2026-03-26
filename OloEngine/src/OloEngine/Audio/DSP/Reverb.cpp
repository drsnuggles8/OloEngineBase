#include "OloEnginePCH.h"
#include "OloEngine/Audio/DSP/Reverb.h"
#include "OloEngine/Audio/DSP/ReverbModel.h"
#include "OloEngine/Audio/DSP/DelayLine.h"

#include <miniaudio.h>

namespace OloEngine::Audio::DSP
{
    struct Reverb::ReverbNode
    {
        ma_node_base base{}; // Must be first member
        DelayLine* delayLine = nullptr;
        ReverbModel* reverb = nullptr;
    };

    void ReverbNodeProcessPcmFrames(void* pNode, const float** ppFramesIn, unsigned int* pFrameCountIn,
                                    float** ppFramesOut, unsigned int* pFrameCountOut)
    {
        auto* node = static_cast<Reverb::ReverbNode*>(pNode);
        auto* pNodeBase = &node->base;
        u32 outBuses = ma_node_get_output_bus_count(pNodeBase);

        // If no input connected, silence the output
        if (ppFramesIn == nullptr)
        {
            for (u32 oBus = 0; oBus < outBuses; ++oBus)
            {
                ma_silence_pcm_frames(ppFramesOut[oBus], *pFrameCountOut, ma_format_f32,
                                      ma_node_get_output_channels(pNodeBase, oBus));
            }
            return;
        }

        const float* pFramesIn0 = ppFramesIn[0];
        float* pFramesOut0 = ppFramesOut[0];

        u32 channels = ma_node_get_input_channels(pNodeBase, 0);
        OLO_CORE_ASSERT(channels == 2);

        // 1. Feed through pre-delay
        auto* delay = node->delayLine;
        const float* channelDataL = &pFramesIn0[0];
        const float* channelDataR = &pFramesIn0[1];
        float* outputL = &pFramesOut0[0];
        float* outputR = &pFramesOut0[1];

        int numSamples = static_cast<int>(*pFrameCountIn);
        while (numSamples-- > 0)
        {
            float delayedL = delay->PopSample(0);
            float delayedR = delay->PopSample(1);
            *outputL = delayedL;
            *outputR = delayedR;
            delay->PushSample(0, *channelDataL);
            delay->PushSample(1, *channelDataR);

            channelDataL += channels;
            channelDataR += channels;
            outputL += channels;
            outputR += channels;
        }

        // 2. Process delayed signal through reverb
        node->reverb->ProcessReplace(pFramesOut0, &pFramesOut0[1], pFramesOut0, &pFramesOut0[1],
                                     static_cast<long>(*pFrameCountIn), static_cast<int>(channels));
    }

    static ma_node_vtable s_ReverbVtable = {
        [](ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
        {
            ReverbNodeProcessPcmFrames(pNode, ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
        },
        nullptr,
        2, // 2 input buses (1 audio + 1 unused/modulator)
        1, // 1 output bus
        MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT
    };

    Reverb::Reverb()
        : m_Node(new ReverbNode())
    {
    }

    Reverb::~Reverb()
    {
        Uninitialize();
        delete m_Node;
        m_Node = nullptr;
    }

    bool Reverb::Initialize(ma_engine* engine, ma_node_base* nodeToAttachTo)
    {
        OLO_CORE_ASSERT(!m_Initialized);

        double sampleRate = ma_engine_get_sample_rate(engine);
        u8 numChannels = static_cast<u8>(ma_node_get_output_channels(nodeToAttachTo, 0));

        m_DelayLine = std::make_unique<DelayLine>(static_cast<int>(sampleRate) + 1);
        m_RevModel = std::make_unique<ReverbModel>(sampleRate);

        ma_uint32 inChannels[2]{ numChannels, numChannels };
        ma_uint32 outChannels[1]{ numChannels };
        ma_node_config nodeConfig = ma_node_config_init();
        nodeConfig.vtable = &s_ReverbVtable;
        nodeConfig.pInputChannels = inChannels;
        nodeConfig.pOutputChannels = outChannels;
        nodeConfig.initialState = ma_node_state_started;

        ma_allocation_callbacks allocationCallbacks = engine->pResourceManager->config.allocationCallbacks;

        ma_result result = ma_node_init(&engine->nodeGraph, &nodeConfig, &allocationCallbacks, m_Node);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[Reverb] Node init failed: {}", static_cast<int>(result));
            return false;
        }

        m_DelayLine->SetConfig(ma_node_get_input_channels(&m_Node->base, 0), sampleRate);
        m_DelayLine->SetDelayMs(50); // Default 50ms pre-delay

        m_Node->delayLine = m_DelayLine.get();
        m_Node->reverb = m_RevModel.get();

        result = ma_node_attach_output_bus(m_Node, 0, nodeToAttachTo, 0);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[Reverb] Node attach failed: {}", static_cast<int>(result));
            ma_node_uninit(m_Node, nullptr);
            return false;
        }

        m_Initialized = true;
        return true;
    }

    void Reverb::Uninitialize()
    {
        if (m_Initialized)
        {
            if (m_Node && m_Node->base.vtable != nullptr)
            {
                ma_node_uninit(m_Node, nullptr);
            }
        }
        m_Initialized = false;
    }

    ma_node_base* Reverb::GetNode()
    {
        return m_Node ? &m_Node->base : nullptr;
    }

    void Reverb::SetParameter(ReverbParameter parameter, float value)
    {
        switch (parameter)
        {
            case ReverbParameter::PreDelay:
                OLO_CORE_ASSERT(value <= m_MaxPreDelay);
                m_DelayLine->SetDelayMs(static_cast<u32>(value));
                break;
            case ReverbParameter::Mode:
                m_RevModel->SetMode(value);
                break;
            case ReverbParameter::RoomSize:
                m_RevModel->SetRoomSize(value);
                break;
            case ReverbParameter::Damp:
                m_RevModel->SetDamp(value);
                break;
            case ReverbParameter::Width:
                m_RevModel->SetWidth(value);
                break;
            case ReverbParameter::Wet:
                m_RevModel->SetWet(value);
                break;
            case ReverbParameter::Dry:
                m_RevModel->SetDry(value);
                break;
            default:
                break;
        }
    }

    float Reverb::GetParameter(ReverbParameter parameter) const
    {
        switch (parameter)
        {
            case ReverbParameter::PreDelay:
                return static_cast<float>(m_DelayLine->GetDelayMs());
            case ReverbParameter::Mode:
                return m_RevModel->GetMode();
            case ReverbParameter::RoomSize:
                return m_RevModel->GetRoomSize();
            case ReverbParameter::Damp:
                return m_RevModel->GetDamp();
            case ReverbParameter::Width:
                return m_RevModel->GetWidth();
            case ReverbParameter::Wet:
                return m_RevModel->GetWet();
            case ReverbParameter::Dry:
                return m_RevModel->GetDry();
            default:
                return 0.0f;
        }
    }
} // namespace OloEngine::Audio::DSP
