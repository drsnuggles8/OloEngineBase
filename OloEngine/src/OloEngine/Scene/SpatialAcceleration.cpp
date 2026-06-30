#include "OloEnginePCH.h"
#include "SpatialAcceleration.h"

#include "OloEngine/Debug/Profiler.h"

#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace OloEngine
{
    SceneSpatialIndex::SceneSpatialIndex(f32 cellSize)
    {
        if (cellSize <= 0.0f)
        {
            OLO_CORE_WARN("SceneSpatialIndex: invalid cellSize {}, clamping to 1.0f", cellSize);
            cellSize = 1.0f;
        }
        m_CellSize = cellSize;
        m_InvCellSize = 1.0f / cellSize;
    }

    void SceneSpatialIndex::Clear()
    {
        // Keep the map's buckets and the entry vector's capacity allocated —
        // the next rebuild refills both to roughly the same size, so clearing
        // (rather than freeing) avoids per-tick reallocation churn.
        m_Entries.clear();
        for (auto& [key, cell] : m_Cells)
        {
            cell.clear();
        }
    }

    void SceneSpatialIndex::Insert(UUID id, const glm::vec3& position)
    {
        // A NaN/Inf position would make std::floor(pos * inv) produce a garbage
        // cell key (and never match any query), so drop it rather than poison
        // the grid. Mirrors the float-validation discipline used for any value
        // that could originate from physics/script divergence.
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        {
            return;
        }

        const u32 index = static_cast<u32>(m_Entries.size());
        m_Entries.push_back(Entry{ id, position });
        m_Cells[PositionToCell(position)].push_back(index);
    }

    std::vector<UUID> SceneSpatialIndex::QueryRadius(const glm::vec3& center, f32 radius) const
    {
        std::vector<UUID> result;
        QueryRadius(center, radius, result);
        return result;
    }

    void SceneSpatialIndex::QueryRadius(const glm::vec3& center, f32 radius, std::vector<UUID>& out) const
    {
        OLO_PROFILE_FUNCTION();

        if (radius < 0.0f)
        {
            return;
        }

        const f32 radiusSq = radius * radius;

        // Only the cells overlapping the query sphere's bounding box can hold a
        // hit — visit those and distance-test each occupant exactly.
        const i32 minX = static_cast<i32>(std::floor((center.x - radius) * m_InvCellSize));
        const i32 maxX = static_cast<i32>(std::floor((center.x + radius) * m_InvCellSize));
        const i32 minY = static_cast<i32>(std::floor((center.y - radius) * m_InvCellSize));
        const i32 maxY = static_cast<i32>(std::floor((center.y + radius) * m_InvCellSize));
        const i32 minZ = static_cast<i32>(std::floor((center.z - radius) * m_InvCellSize));
        const i32 maxZ = static_cast<i32>(std::floor((center.z + radius) * m_InvCellSize));

        for (i32 x = minX; x <= maxX; ++x)
        {
            for (i32 y = minY; y <= maxY; ++y)
            {
                for (i32 z = minZ; z <= maxZ; ++z)
                {
                    auto cellIt = m_Cells.find(CellKey{ x, y, z });
                    if (cellIt == m_Cells.end())
                    {
                        continue;
                    }

                    for (u32 index : cellIt->second)
                    {
                        const Entry& entry = m_Entries[index];
                        if (glm::distance2(center, entry.Position) <= radiusSq)
                        {
                            out.push_back(entry.Id);
                        }
                    }
                }
            }
        }
    }

    std::vector<UUID> SceneSpatialIndex::QueryAABB(const glm::vec3& min, const glm::vec3& max) const
    {
        std::vector<UUID> result;
        QueryAABB(min, max, result);
        return result;
    }

    void SceneSpatialIndex::QueryAABB(const glm::vec3& min, const glm::vec3& max, std::vector<UUID>& out) const
    {
        OLO_PROFILE_FUNCTION();

        if (min.x > max.x || min.y > max.y || min.z > max.z)
        {
            return; // degenerate / inverted box holds nothing
        }

        const i32 minX = static_cast<i32>(std::floor(min.x * m_InvCellSize));
        const i32 maxX = static_cast<i32>(std::floor(max.x * m_InvCellSize));
        const i32 minY = static_cast<i32>(std::floor(min.y * m_InvCellSize));
        const i32 maxY = static_cast<i32>(std::floor(max.y * m_InvCellSize));
        const i32 minZ = static_cast<i32>(std::floor(min.z * m_InvCellSize));
        const i32 maxZ = static_cast<i32>(std::floor(max.z * m_InvCellSize));

        for (i32 x = minX; x <= maxX; ++x)
        {
            for (i32 y = minY; y <= maxY; ++y)
            {
                for (i32 z = minZ; z <= maxZ; ++z)
                {
                    auto cellIt = m_Cells.find(CellKey{ x, y, z });
                    if (cellIt == m_Cells.end())
                    {
                        continue;
                    }

                    for (u32 index : cellIt->second)
                    {
                        const Entry& entry = m_Entries[index];
                        const glm::vec3& p = entry.Position;
                        if (p.x >= min.x && p.x <= max.x &&
                            p.y >= min.y && p.y <= max.y &&
                            p.z >= min.z && p.z <= max.z)
                        {
                            out.push_back(entry.Id);
                        }
                    }
                }
            }
        }
    }

    std::vector<UUID> SceneSpatialIndex::NearestN(const glm::vec3& center, u32 count, f32 maxRadius) const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<UUID> result;
        if (count == 0 || m_Entries.empty() || maxRadius < 0.0f)
        {
            return result;
        }

        // (distance², UUID) candidate set. Bounded searches walk the cell range
        // around the sphere; an unbounded search (sentinel maxRadius) would have
        // to visit the whole cell range anyway, so scan the flat entry list
        // directly — same O(n) cost without the cell-iteration overhead.
        std::vector<std::pair<f32, UUID>> candidates;

        const bool bounded = maxRadius != std::numeric_limits<f32>::max();
        if (bounded)
        {
            const f32 maxRadiusSq = maxRadius * maxRadius;

            const i32 minX = static_cast<i32>(std::floor((center.x - maxRadius) * m_InvCellSize));
            const i32 maxX = static_cast<i32>(std::floor((center.x + maxRadius) * m_InvCellSize));
            const i32 minY = static_cast<i32>(std::floor((center.y - maxRadius) * m_InvCellSize));
            const i32 maxY = static_cast<i32>(std::floor((center.y + maxRadius) * m_InvCellSize));
            const i32 minZ = static_cast<i32>(std::floor((center.z - maxRadius) * m_InvCellSize));
            const i32 maxZ = static_cast<i32>(std::floor((center.z + maxRadius) * m_InvCellSize));

            for (i32 x = minX; x <= maxX; ++x)
            {
                for (i32 y = minY; y <= maxY; ++y)
                {
                    for (i32 z = minZ; z <= maxZ; ++z)
                    {
                        auto cellIt = m_Cells.find(CellKey{ x, y, z });
                        if (cellIt == m_Cells.end())
                        {
                            continue;
                        }
                        for (u32 index : cellIt->second)
                        {
                            const Entry& entry = m_Entries[index];
                            const f32 distSq = glm::distance2(center, entry.Position);
                            if (distSq <= maxRadiusSq)
                            {
                                candidates.emplace_back(distSq, entry.Id);
                            }
                        }
                    }
                }
            }
        }
        else
        {
            candidates.reserve(m_Entries.size());
            for (const Entry& entry : m_Entries)
            {
                candidates.emplace_back(glm::distance2(center, entry.Position), entry.Id);
            }
        }

        const sizet n = std::min<sizet>(count, candidates.size());
        // Partial sort keeps the cost O(m log n) instead of fully sorting every
        // candidate when only the closest n are wanted. Compare on distance,
        // then UUID, so equidistant entries order deterministically.
        std::partial_sort(candidates.begin(), candidates.begin() + n, candidates.end(),
                          [](const std::pair<f32, UUID>& a, const std::pair<f32, UUID>& b)
                          {
                              if (a.first != b.first)
                              {
                                  return a.first < b.first;
                              }
                              return static_cast<u64>(a.second) < static_cast<u64>(b.second);
                          });

        result.reserve(n);
        for (sizet i = 0; i < n; ++i)
        {
            result.push_back(candidates[i].second);
        }
        return result;
    }

    void SceneSpatialIndex::SetCellSize(f32 cellSize)
    {
        if (cellSize <= 0.0f)
        {
            return;
        }
        if (std::fabs(cellSize - m_CellSize) < 1e-6f)
        {
            return;
        }

        m_CellSize = cellSize;
        m_InvCellSize = 1.0f / cellSize;

        // Re-bin every existing entry under the new resolution.
        auto entries = std::move(m_Entries);
        m_Entries.clear();
        m_Cells.clear();
        for (const Entry& entry : entries)
        {
            Insert(entry.Id, entry.Position);
        }
    }

    SceneSpatialIndex::CellKey SceneSpatialIndex::PositionToCell(const glm::vec3& pos) const
    {
        return { static_cast<i32>(std::floor(pos.x * m_InvCellSize)),
                 static_cast<i32>(std::floor(pos.y * m_InvCellSize)),
                 static_cast<i32>(std::floor(pos.z * m_InvCellSize)) };
    }
} // namespace OloEngine
