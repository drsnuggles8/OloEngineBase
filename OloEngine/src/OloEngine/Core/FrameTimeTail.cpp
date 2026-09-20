#include "OloEnginePCH.h"

#include "OloEngine/Core/FrameTimeTail.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kMinCapacity = 2u;
        constexpr u32 kMaxCapacity = 1u << 20;
    } // namespace

    FrameTimeTail::FrameTimeTail(u32 capacity)
    {
        m_Samples.assign(std::clamp(capacity, kMinCapacity, kMaxCapacity), 0.0f);
    }

    void FrameTimeTail::Push(f32 frameMs)
    {
        if (!std::isfinite(frameMs) || frameMs < 0.0f)
        {
            // DROPPED AND COUNTED, never clamped. A zero substituted for a
            // broken delta would pull every percentile down, so a clock fault
            // would read as a performance win — and the counter is the only
            // thing that would ever say otherwise.
            ++m_Rejected;
            return;
        }
        m_Samples[m_Head] = frameMs;
        m_Head = (m_Head + 1u) % static_cast<u32>(m_Samples.size());
        if (m_Count < static_cast<u32>(m_Samples.size()))
        {
            ++m_Count;
        }
    }

    void FrameTimeTail::Reset()
    {
        m_Head = 0u;
        m_Count = 0u;
        m_Rejected = 0u;
        std::fill(m_Samples.begin(), m_Samples.end(), 0.0f);
    }

    FrameTimeTailStats FrameTimeTail::Query(f32 budgetMs) const
    {
        FrameTimeTailStats stats;
        stats.BudgetMs = (std::isfinite(budgetMs) && budgetMs > 0.0f) ? budgetMs : 0.0f;
        if (m_Count == 0u)
        {
            return stats;
        }

        // Only the OCCUPIED prefix of the ring, not the whole buffer. A window
        // that has seen forty frames of a six-hundred-frame ring must report
        // forty samples; including the zero-filled remainder would put p50 at
        // zero and read as a spectacularly fast frame.
        std::vector<f32> sorted;
        sorted.reserve(m_Count);
        const u32 capacity = static_cast<u32>(m_Samples.size());
        const u32 first = (m_Count == capacity) ? m_Head : 0u;
        for (u32 i = 0; i < m_Count; ++i)
        {
            sorted.push_back(m_Samples[(first + i) % capacity]);
        }

        f32 sum = 0.0f;
        for (const f32 v : sorted)
        {
            sum += v;
            if (stats.BudgetMs > 0.0f && v > stats.BudgetMs)
            {
                ++stats.OverBudgetFrames;
            }
        }

        std::sort(sorted.begin(), sorted.end());

        stats.SampleCount = m_Count;
        stats.MinMs = sorted.front();
        stats.MaxMs = sorted.back();
        stats.MeanMs = sum / static_cast<f32>(m_Count);
        stats.P50Ms = NearestRankPercentile(sorted, 0.50f);
        stats.P95Ms = NearestRankPercentile(sorted, 0.95f);
        stats.P99Ms = NearestRankPercentile(sorted, 0.99f);
        return stats;
    }

    f32 NearestRankPercentile(std::span<const f32> sortedAscending, f32 q) noexcept
    {
        if (sortedAscending.empty() || !std::isfinite(q) || q < 0.0f || q > 1.0f)
        {
            return 0.0f;
        }
        const sizet n = sortedAscending.size();
        // ceil(q * n), 1-indexed, clamped to [1, n] — perf_trend.py's
        // definition exactly, so the in-engine number and the offline trend
        // number are the same statistic rather than two that nearly agree.
        const f64 rank = std::ceil(static_cast<f64>(q) * static_cast<f64>(n));
        const sizet index = rank < 1.0 ? 0u : static_cast<sizet>(rank) - 1u;
        return sortedAscending[std::min(index, n - 1u)];
    }
} // namespace OloEngine
