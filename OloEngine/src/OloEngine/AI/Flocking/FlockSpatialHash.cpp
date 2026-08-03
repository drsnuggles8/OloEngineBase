#include "OloEnginePCH.h"

#include "OloEngine/AI/Flocking/FlockSpatialHash.h"

#include <cmath>

namespace OloEngine
{
    void FlockSpatialHash::Clear()
    {
        m_Positions.clear();
        m_Cells.clear();
        m_Items.clear();
        m_BucketStart.clear();
        m_BucketCount = 0;
        m_BucketMask = 0;
        m_IndexedCount = 0;
        m_OccupiedBuckets = 0;
        m_MaxBucketLoad = 0;
    }

    void FlockSpatialHash::Rebuild(std::span<const glm::vec3> positions, f32 cellSize)
    {
        OLO_PROFILE_FUNCTION();

        m_CellSize = (std::isfinite(cellSize) && cellSize > kMinCellSize) ? cellSize : kMinCellSize;
        m_InvCellSize = 1.0f / m_CellSize;

        const sizet count = positions.size();
        m_Positions.assign(positions.begin(), positions.end());
        m_Cells.assign(count, CellCoord{});
        m_IndexedCount = 0;
        m_OccupiedBuckets = 0;
        m_MaxBucketLoad = 0;

        if (count == 0)
        {
            m_Items.clear();
            m_BucketStart.clear();
            m_BucketCount = 0;
            m_BucketMask = 0;
            return;
        }

        // ~2 buckets per item keeps the average bucket load well below one
        // while staying a power of two (mask-reducible).
        u32 buckets = kMinBuckets;
        while (buckets < kMaxBuckets && static_cast<sizet>(buckets) < count * 2)
            buckets <<= 1;
        m_BucketCount = buckets;
        m_BucketMask = buckets - 1;

        // Pass 1 — classify each item and count its bucket. The counts are
        // written at [bucket + 1] so the prefix sum below turns the array
        // directly into CSR start offsets.
        m_BucketStart.assign(static_cast<sizet>(buckets) + 1, 0u);
        for (sizet i = 0; i < count; ++i)
        {
            const glm::vec3& p = positions[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                continue; // never indexed, so never returned as a neighbour

            const CellCoord cell = ToCell(p);
            m_Cells[i] = cell;
            ++m_BucketStart[static_cast<sizet>(BucketOf(cell)) + 1];
            ++m_IndexedCount;
        }

        for (u32 b = 0; b < buckets; ++b)
        {
            const u32 load = m_BucketStart[static_cast<sizet>(b) + 1];
            if (load > 0)
            {
                ++m_OccupiedBuckets;
                if (load > m_MaxBucketLoad)
                    m_MaxBucketLoad = load;
            }
            m_BucketStart[static_cast<sizet>(b) + 1] += m_BucketStart[b];
        }

        // Pass 2 — stable scatter in ascending item index, so every bucket's
        // members come out sorted and the traversal order is reproducible.
        m_Cursor.assign(m_BucketStart.begin(), m_BucketStart.end() - 1);
        m_Items.assign(m_IndexedCount, 0u);
        for (sizet i = 0; i < count; ++i)
        {
            const glm::vec3& p = positions[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                continue;

            m_Items[m_Cursor[BucketOf(m_Cells[i])]++] = static_cast<u32>(i);
        }
    }
} // namespace OloEngine
