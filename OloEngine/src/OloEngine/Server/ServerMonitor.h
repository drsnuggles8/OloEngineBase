#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Timer.h"

namespace OloEngine
{
    // Periodic server-side monitoring — logs tick timing, connection count,
    // and network bandwidth at a configurable interval.
    class ServerMonitor
    {
      public:
        // reportIntervalSeconds: how often to log a summary (default 30 s)
        // tickBudgetSeconds: tick duration threshold for budget overrun detection
        explicit ServerMonitor(f32 reportIntervalSeconds = 30.0f, f32 tickBudgetSeconds = 0.0f);
        ~ServerMonitor() = default;

        // Call once per tick with the measured tick execution duration.
        void RecordTick(f32 tickDurationSeconds);

        // Periodic report is printed automatically from RecordTick when the
        // interval elapses.  Call ForceReport() to print immediately and
        // optionally reset accumulators.
        void ForceReport(bool resetAccumulators = true);

        void SetReportInterval(f32 seconds);
        void SetTickBudget(f32 budgetSeconds);

      private:
        void PrintReport() const;
        void ResetAccumulators();

        // Configuration
        f32 m_ReportInterval;
        f32 m_TickBudget;

        // Accumulation between reports
        Timer m_ReportTimer;
        u64 m_TickCount = 0;
        f32 m_TotalTickDuration = 0.0f;
        f32 m_MaxTickDuration = 0.0f;
        u32 m_BudgetOverruns = 0;
    };
} // namespace OloEngine
