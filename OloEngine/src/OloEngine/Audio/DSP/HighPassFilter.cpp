#include "OloEnginePCH.h"
#include "OloEngine/Audio/DSP/HighPassFilter.h"

#include <miniaudio.h>

namespace OloEngine::Audio::DSP
{
    struct HighPassFilter::Impl
    {
        ma_hpf_node node{};
    };

    static constexpr u32 FilterOrder = 2;

    HighPassFilter::HighPassFilter() = default;

    HighPassFilter::~HighPassFilter()
    {
        Uninitialize();
    }

    bool HighPassFilter::Initialize(ma_engine* engine, ma_node_base* nodeToInsertAfter)
    {
        OLO_CORE_ASSERT(!m_Initialized);

        m_SampleRate = ma_engine_get_sample_rate(engine);
        m_Channels = ma_node_get_output_channels(nodeToInsertAfter, 0);

        m_Impl = new Impl();

        // Initialize with a safe default frequency (20 Hz) to avoid div-by-zero;
        // the caller will set the actual cutoff via SetCutoffFrequency right after.
        double initCutoff = m_CutoffFrequency.load();
        if (initCutoff <= 0.0)
        {
            initCutoff = 20.0;
        }

        ma_hpf_node_config config = ma_hpf_node_config_init(
            m_Channels, static_cast<ma_uint32>(m_SampleRate), initCutoff, FilterOrder);

        ma_result result = ma_hpf_node_init(&engine->nodeGraph, &config, nullptr, &m_Impl->node);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[HighPassFilter] Node init failed: {}", static_cast<int>(result));
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
            OLO_CORE_ERROR("[HighPassFilter] Output attach failed: {}", static_cast<int>(result));
            Uninitialize();
            return false;
        }

        result = ma_node_attach_output_bus(nodeToInsertAfter, 0, &m_Impl->node, 0);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[HighPassFilter] Input attach failed: {}", static_cast<int>(result));
            Uninitialize();
            return false;
        }

        m_Initialized = true;
        return true;
    }

    void HighPassFilter::Uninitialize()
    {
        if (m_Initialized && m_Impl)
        {
            ma_hpf_node_uninit(&m_Impl->node, nullptr);
        }
        delete m_Impl;
        m_Impl = nullptr;
        m_Initialized = false;
    }

    ma_node_base* HighPassFilter::GetNode()
    {
        return m_Impl ? &m_Impl->node.baseNode : nullptr;
    }

    void HighPassFilter::SetCutoffFrequency(double frequency)
    {
        if (m_CutoffFrequency == frequency)
        {
            return;
        }
        m_CutoffFrequency = frequency;

        if (m_Impl)
        {
            ma_hpf_config config = ma_hpf_config_init(
                ma_format_f32, m_Channels, static_cast<ma_uint32>(m_SampleRate), frequency, FilterOrder);
            ma_hpf_node_reinit(&config, &m_Impl->node);
        }
    }

    void HighPassFilter::SetCutoffValue(double cutoffNormalized)
    {
        double cutoffFrequency = cutoffNormalized * 20000.0;
        SetCutoffFrequency(cutoffFrequency);
    }
} // namespace OloEngine::Audio::DSP
