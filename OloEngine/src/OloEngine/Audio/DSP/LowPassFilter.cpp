#include "OloEnginePCH.h"
#include "OloEngine/Audio/DSP/LowPassFilter.h"

#include <miniaudio.h>

namespace OloEngine::Audio::DSP
{
    struct LowPassFilter::Impl
    {
        ma_lpf_node node{};
    };

    static constexpr u32 FilterOrder = 2;

    LowPassFilter::LowPassFilter() = default;

    LowPassFilter::~LowPassFilter()
    {
        Uninitialize();
    }

    bool LowPassFilter::Initialize(ma_engine* engine, ma_node_base* nodeToInsertAfter)
    {
        OLO_CORE_ASSERT(!m_Initialized);

        m_SampleRate = ma_engine_get_sample_rate(engine);
        m_Channels = ma_node_get_output_channels(nodeToInsertAfter, 0);

        m_Impl = new Impl();

        ma_lpf_node_config config = ma_lpf_node_config_init(
            m_Channels, static_cast<ma_uint32>(m_SampleRate), m_CutoffFrequency.load(), FilterOrder);

        ma_result result = ma_lpf_node_init(&engine->nodeGraph, &config, nullptr, &m_Impl->node);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[LowPassFilter] Node init failed: {}", static_cast<int>(result));
            delete m_Impl;
            m_Impl = nullptr;
            return false;
        }

        // Insert into the graph: source -> [this filter] -> destination
        auto* destination = nodeToInsertAfter->pOutputBuses[0].pInputNode;
        ma_uint8 destInputBus = nodeToInsertAfter->pOutputBuses[0].inputNodeInputBusIndex;

        result = ma_node_attach_output_bus(&m_Impl->node, 0, destination, destInputBus);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[LowPassFilter] Output attach failed: {}", static_cast<int>(result));
            Uninitialize();
            return false;
        }

        result = ma_node_attach_output_bus(nodeToInsertAfter, 0, &m_Impl->node, 0);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[LowPassFilter] Input attach failed: {}", static_cast<int>(result));
            Uninitialize();
            return false;
        }

        m_Initialized = true;
        return true;
    }

    void LowPassFilter::Uninitialize()
    {
        if (m_Initialized && m_Impl)
        {
            ma_lpf_node_uninit(&m_Impl->node, nullptr);
        }
        delete m_Impl;
        m_Impl = nullptr;
        m_Initialized = false;
    }

    ma_node_base* LowPassFilter::GetNode()
    {
        return m_Impl ? &m_Impl->node.baseNode : nullptr;
    }

    void LowPassFilter::SetCutoffFrequency(double frequency)
    {
        if (m_CutoffFrequency == frequency)
        {
            return;
        }
        m_CutoffFrequency = frequency;

        if (m_Impl)
        {
            ma_lpf_config config = ma_lpf_config_init(
                ma_format_f32, m_Channels, static_cast<ma_uint32>(m_SampleRate), frequency, FilterOrder);
            ma_lpf_node_reinit(&config, &m_Impl->node);
        }
    }

    void LowPassFilter::SetCutoffValue(double cutoffNormalized)
    {
        double cutoffFrequency = cutoffNormalized * 20000.0;
        SetCutoffFrequency(cutoffFrequency);
    }
} // namespace OloEngine::Audio::DSP
