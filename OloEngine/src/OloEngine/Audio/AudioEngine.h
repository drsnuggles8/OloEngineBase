#pragma once

#include "OloEngine/HAL/Thread.h"
#include <atomic>
#include <optional>

struct ma_engine;

namespace OloEngine
{
    namespace Audio::DSP
    {
        class Reverb;
        class Spatializer;
        enum class ReverbParameter : u8;
    } // namespace Audio::DSP

    using AudioEngineInternal = void*;

    struct AudioStats
    {
        u32 SampleRate = 0;
        bool ReverbAvailable = false;
        bool SpatializerAvailable = false;
        bool AudioThreadRunning = false;
    };

    class AudioEngine
    {
      public:
        static bool Init();
        static void Shutdown();

        [[nodiscard("Store this!")]] static AudioEngineInternal GetEngine();

        // Master reverb bus
        [[nodiscard("Store this!")]] static Audio::DSP::Reverb* GetMasterReverb();
        static void SetMasterReverbParameter(Audio::DSP::ReverbParameter parameter, float value);
        [[nodiscard("Store this!")]] static std::optional<float> GetMasterReverbParameter(Audio::DSP::ReverbParameter parameter);

        // 3D spatializer
        [[nodiscard("Store this!")]] static Audio::DSP::Spatializer* GetSpatializer();

        // Statistics
        [[nodiscard("Store this!")]] static AudioStats GetStats();

        // Audio thread utilities
        static bool IsAudioThread();

      private:
        static void AudioThreadFunc();

      private:
        static ma_engine* s_Engine;
        static FThread s_AudioThread;
        static std::atomic<bool> s_AudioThreadRunning;
        static Audio::DSP::Reverb* s_MasterReverb;
        static Audio::DSP::Spatializer* s_Spatializer;
    };
} // namespace OloEngine
