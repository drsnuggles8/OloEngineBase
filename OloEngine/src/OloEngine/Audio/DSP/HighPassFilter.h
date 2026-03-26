#pragma once

// High-pass filter wrapping miniaudio's built-in ma_hpf_node.
// Inserts into the audio graph between a source node and the endpoint.

#include <atomic>

struct ma_engine;
struct ma_node_base;

namespace OloEngine::Audio::DSP
{
    class HighPassFilter
    {
      public:
        HighPassFilter();
        ~HighPassFilter();

        HighPassFilter(const HighPassFilter&) = delete;
        HighPassFilter& operator=(const HighPassFilter&) = delete;

        bool Initialize(ma_engine* engine, ma_node_base* nodeToInsertAfter);
        void Uninitialize();
        [[nodiscard]] ma_node_base* GetNode();

        // Set cutoff as a normalized value [0,1] (maps to 20 Hz – 20 kHz)
        void SetCutoffValue(double cutoffNormalized);
        // Set cutoff directly in Hz
        void SetCutoffFrequency(double frequency);
        [[nodiscard]] double GetCutoffFrequency() const { return m_CutoffFrequency.load(); }

      private:
        struct Impl; // Wraps ma_hpf_node (hidden from header)

        bool m_Initialized = false;
        u32 m_Channels = 0;
        double m_SampleRate = 0.0;
        std::atomic<double> m_CutoffFrequency = 0.0;
        Impl* m_Impl = nullptr;
    };
} // namespace OloEngine::Audio::DSP
