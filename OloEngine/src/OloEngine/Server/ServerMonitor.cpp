#include "OloEnginePCH.h"
#include "ServerMonitor.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Networking/Core/NetworkManager.h"
#include "OloEngine/Networking/Transport/NetworkServer.h"

namespace OloEngine
{
    ServerMonitor::ServerMonitor(f32 reportIntervalSeconds)
        : m_ReportInterval(reportIntervalSeconds)
    {
    }

    void ServerMonitor::RecordTick(f32 tickDurationSeconds)
    {
        ++m_TickCount;
        m_TotalTickDuration += tickDurationSeconds;
        m_MaxTickDuration = std::max(m_MaxTickDuration, tickDurationSeconds);

        if (m_ReportTimer.Elapsed() >= m_ReportInterval)
        {
            PrintReport();

            // Reset accumulators
            m_TickCount = 0;
            m_TotalTickDuration = 0.0f;
            m_MaxTickDuration = 0.0f;
            m_BudgetOverruns = 0;
            m_ReportTimer.Reset();
        }
    }

    void ServerMonitor::SetReportInterval(f32 seconds)
    {
        m_ReportInterval = seconds;
    }

    void ServerMonitor::ForceReport() const
    {
        PrintReport();
    }

    void ServerMonitor::PrintReport() const
    {
        if (m_TickCount == 0)
        {
            return;
        }

        const f32 avgTickMs = (m_TotalTickDuration / static_cast<f32>(m_TickCount)) * 1000.0f;
        const f32 maxTickMs = m_MaxTickDuration * 1000.0f;

        OLO_CORE_INFO("=== Server Monitor Report ===");
        OLO_CORE_INFO("  Ticks: {}  |  Avg: {:.2f} ms  |  Max: {:.2f} ms", m_TickCount, avgTickMs, maxTickMs);

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
