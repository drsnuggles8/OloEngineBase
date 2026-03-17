#include "OloEnginePCH.h"
#include "ServerMonitor.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"

namespace OloEngine
{
    ServerMonitor::ServerMonitor(f32 reportIntervalSeconds, f32 tickBudgetSeconds)
        : m_ReportInterval(reportIntervalSeconds), m_TickBudget(tickBudgetSeconds)
    {
    }

    void ServerMonitor::RecordTick(f32 tickDurationSeconds)
    {
        OLO_PROFILE_FUNCTION();

        ++m_TickCount;
        m_TotalTickDuration += tickDurationSeconds;
        m_MaxTickDuration = std::max(m_MaxTickDuration, tickDurationSeconds);

        if (m_TickBudget > 0.0f && tickDurationSeconds > m_TickBudget)
        {
            ++m_BudgetOverruns;
        }

        if (m_ReportTimer.Elapsed() >= m_ReportInterval)
        {
            PrintReport();
            ResetAccumulators();
            m_ReportTimer.Reset();
        }
    }

    void ServerMonitor::SetReportInterval(f32 seconds)
    {
        OLO_PROFILE_FUNCTION();
        m_ReportInterval = seconds;
    }

    void ServerMonitor::SetTickBudget(f32 budgetSeconds)
    {
        OLO_PROFILE_FUNCTION();
        m_TickBudget = budgetSeconds;
    }

    void ServerMonitor::ForceReport(bool resetAccumulators)
    {
        OLO_PROFILE_FUNCTION();

        PrintReport();
        if (resetAccumulators)
        {
            ResetAccumulators();
            m_ReportTimer.Reset();
        }
    }

    void ServerMonitor::ResetAccumulators()
    {
        OLO_PROFILE_FUNCTION();

        m_TickCount = 0;
        m_TotalTickDuration = 0.0f;
        m_MaxTickDuration = 0.0f;
        m_BudgetOverruns = 0;
    }

    void ServerMonitor::PrintReport() const
    {
        OLO_PROFILE_FUNCTION();

        if (m_TickCount == 0)
        {
            return;
        }

        const f32 avgTickMs = (m_TotalTickDuration / static_cast<f32>(m_TickCount)) * 1000.0f;
        const f32 maxTickMs = m_MaxTickDuration * 1000.0f;

        OLO_CORE_INFO("=== Server Monitor Report ===");
        OLO_CORE_INFO("  Ticks: {}  |  Avg: {:.2f} ms  |  Max: {:.2f} ms", m_TickCount, avgTickMs, maxTickMs);

        if (m_BudgetOverruns > 0)
        {
            OLO_CORE_INFO("  Budget overruns: {}", m_BudgetOverruns);
        }

        // Network stats (if the server is running)
        if (auto* server = NetworkManager::GetServer(); server)
        {
            const u32 connections = server->GetConnectionCount();
            const auto stats = server->GetStats();

            OLO_CORE_INFO("  Connections: {}", connections);
            OLO_CORE_INFO("  Net send: {:.1f} KB/s  |  recv: {:.1f} KB/s",
                          stats.BytesSentPerSec / 1024.0f,
                          stats.BytesReceivedPerSec / 1024.0f);
            OLO_CORE_INFO("  Messages sent/s: {:.0f}  |  recv/s: {:.0f}",
                          stats.MessagesSentPerSec, stats.MessagesReceivedPerSec);
        }

        OLO_CORE_INFO("=============================");
    }

} // namespace OloEngine
