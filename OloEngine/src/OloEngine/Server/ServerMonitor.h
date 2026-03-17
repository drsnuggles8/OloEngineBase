#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timer.h"

namespace OloEngine
{
    // Periodic server-side monitoring — logs tick timing, connection count,
    // network bandwidth, and memory usage at a configurable interval.
    class ServerMonitor
    {
      public:
        // reportIntervalSeconds: how often to log a summary (default 30 s)
        explicit ServerMonitor(f32 reportIntervalSeconds = 30.0f);
        ~ServerMonitor() = default;

        // Call once per tick with the delta time and the measured tick duration.
        void RecordTick(f32 tickDurationSeconds);

        // Periodic report is printed automatically from RecordTick when the
        // interval elapses.  Call ForceReport() to print immediately.
        void ForceReport() const;

        void SetReportInterval(f32 seconds);

      private:
        void PrintReport() const;

        // Configuration
        f32 m_ReportInterval;

        // Accumulation between reports
        Timer m_ReportTimer;
        u64 m_TickCount = 0;
        f32 m_TotalTickDuration = 0.0f;
        f32 m_MaxTickDuration = 0.0f;
        u32 m_BudgetOverruns = 0;
    };
} // namespace OloEngine
