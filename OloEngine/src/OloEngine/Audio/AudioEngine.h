#pragma once

#include "OloEngine/HAL/Thread.h"
#include <atomic>

struct ma_engine;

namespace OloEngine
{
    using AudioEngineInternal = void*;

    class AudioEngine
    {
      public:
        static bool Init();
        static void Shutdown();

        static AudioEngineInternal GetEngine();

      private:
        static void AudioThreadFunc();

      private:
        static ma_engine* s_Engine;
        static FThread s_AudioThread;
        static std::atomic<bool> s_AudioThreadRunning;
    };
} // namespace OloEngine
