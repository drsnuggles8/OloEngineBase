#pragma once

#include "OloEngine/Core/Timer.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    struct PerFrameData
    {
        f32 Time = 0.0f;    // Accumulated time (ms) for this scope this frame
        u32 Samples = 0;    // Number of invocations this frame
    };

    class PerformanceProfiler
    {
      public:
        void SetPerFrameTiming(const char* name, f32 timeMs)
        {
            std::scoped_lock lock(m_Mutex);
            auto& data = m_CurrentFrameData[std::string(name)];
            data.Time += timeMs;
            ++data.Samples;
        }

        // Snapshot current frame data and reset for the next frame.
        // Call once per frame from the main loop.
        void EndFrame()
        {
            std::scoped_lock lock(m_Mutex);
            m_PreviousFrameData = m_CurrentFrameData;
            m_CurrentFrameData.clear();
        }

        [[nodiscard]] const std::unordered_map<std::string, PerFrameData>& GetPreviousFrameData() const
        {
            // No lock needed — only read by the same thread that called EndFrame()
            return m_PreviousFrameData;
        }

        void Clear()
        {
            std::scoped_lock lock(m_Mutex);
            m_CurrentFrameData.clear();
            m_PreviousFrameData.clear();
        }

      private:
        std::mutex m_Mutex;
        std::unordered_map<std::string, PerFrameData> m_CurrentFrameData;
        std::unordered_map<std::string, PerFrameData> m_PreviousFrameData;
    };

    class ScopedPerformanceTimer
    {
      public:
        ScopedPerformanceTimer(const char* name, PerformanceProfiler* profiler)
            : m_Name(name), m_Profiler(profiler)
        {
        }

        ~ScopedPerformanceTimer()
        {
            if (m_Profiler)
            {
                m_Profiler->SetPerFrameTiming(m_Name, m_Timer.ElapsedMillis());
            }
        }

        ScopedPerformanceTimer(const ScopedPerformanceTimer&) = delete;
        ScopedPerformanceTimer& operator=(const ScopedPerformanceTimer&) = delete;

      private:
        const char* m_Name;
        PerformanceProfiler* m_Profiler;
        Timer m_Timer;
    };
} // namespace OloEngine

// Macros — require a PerformanceProfiler* to be accessible
#if defined(OLO_DEBUG) || defined(OLO_RELEASE)
#define OLO_PERF_SCOPE_LINE2(name, line, profiler) \
    ::OloEngine::ScopedPerformanceTimer olo_perf_timer_##line(name, profiler)
#define OLO_PERF_SCOPE_LINE(name, line, profiler) OLO_PERF_SCOPE_LINE2(name, line, profiler)
#define OLO_PERF_SCOPE(name, profiler) OLO_PERF_SCOPE_LINE(name, __LINE__, profiler)
#define OLO_PERF_FUNCTION(profiler) OLO_PERF_SCOPE(OLO_FUNC_SIG, profiler)
#else
#define OLO_PERF_SCOPE(name, profiler) ((void)0)
#define OLO_PERF_FUNCTION(profiler) ((void)0)
#endif
