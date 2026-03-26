#include "OloEnginePCH.h"
#include "OloEngine/Audio/DSP/LowPassFilter.h"

#include <cmath>
#include <miniaudio.h>

namespace OloEngine::Audio::DSP
{
    struct LowPassFilter::Impl
    {
        ma_lpf_node node{};
    };

    static constexpr u32 FilterOrder = 2;
    static constexpr double MinFrequencyHz = 20.0;
    static constexpr double MaxFrequencyHz = 20000.0;

    LowPassFilter::LowPassFilter() = default;

    LowPassFilter::~LowPassFilter()
    {
        Uninitialize();
    }

    LowPassFilter::LowPassFilter(LowPassFilter&& other) noexcept
        : m_Initialized(other.m_Initialized),
          m_Channels(other.m_Channels),
          m_SampleRate(other.m_SampleRate),
          m_CutoffFrequency(other.m_CutoffFrequency.load()),
          m_Impl(std::move(other.m_Impl))
    {
        other.m_Initialized = false;
    }

    LowPassFilter& LowPassFilter::operator=(LowPassFilter&& other) noexcept
    {
        if (this != &other)
        {
            Uninitialize();
            m_Initialized = other.m_Initialized;
            m_Channels = other.m_Channels;
            m_SampleRate = other.m_SampleRate;
            m_CutoffFrequency.store(other.m_CutoffFrequency.load());
            m_Impl = std::move(other.m_Impl);
            other.m_Initialized = false;
        }
        return *this;
    }

    bool LowPassFilter::Initialize(ma_engine* engine, ma_node_base* nodeToInsertAfter)
    {
        OLO_CORE_ASSERT(!m_Initialized);

        m_SampleRate = ma_engine_get_sample_rate(engine);
        m_Channels = ma_node_get_output_channels(nodeToInsertAfter, 0);

        m_Impl = std::make_unique<Impl>();

        double nyquist = m_SampleRate / 2.0;
        double initCutoff = std::min(m_CutoffFrequency.load(), nyquist);

        ma_lpf_node_config config = ma_lpf_node_config_init(
            m_Channels, static_cast<ma_uint32>(m_SampleRate), initCutoff, FilterOrder);

        ma_result result = ma_lpf_node_init(&engine->nodeGraph, &config, nullptr, &m_Impl->node);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[LowPassFilter] Node init failed: {}", static_cast<int>(result));
            m_Impl = nullptr;
            return false;
        }

        // Mark initialized early so Uninitialize() can clean up the node if attach fails
        m_Initialized = true;

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

        return true;
    }

    void LowPassFilter::Uninitialize()
    {
        if (m_Initialized && m_Impl)
        {
            ma_lpf_node_uninit(&m_Impl->node, nullptr);
        }
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
            double nyquist = m_SampleRate / 2.0;
            double clampedFreq = std::min(frequency, nyquist);
            ma_lpf_config config = ma_lpf_config_init(
                ma_format_f32, m_Channels, static_cast<ma_uint32>(m_SampleRate), clampedFreq, FilterOrder);
            ma_result result = ma_lpf_node_reinit(&config, &m_Impl->node);
            if (result != MA_SUCCESS)
            {
                OLO_CORE_ERROR("[LowPassFilter] Reinit failed: {}", static_cast<int>(result));
            }
        }
    }

    void LowPassFilter::SetCutoffValue(double cutoffNormalized)
    {
        double clamped = std::clamp(cutoffNormalized, 0.0, 1.0);
        double cutoffFrequency = std::exp(std::log(MinFrequencyHz) + clamped * (std::log(MaxFrequencyHz) - std::log(MinFrequencyHz)));
        SetCutoffFrequency(cutoffFrequency);
    }
} // namespace OloEngine::Audio::DSP
