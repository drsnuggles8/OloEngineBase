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
    namespace
    {
        // A position/query point with any non-finite component would make
        // std::floor(v * inv) produce a garbage cell coordinate (and an integer
        // cast of NaN/Inf is UB), so every grid-math entry point screens for it.
        [[nodiscard]] bool IsFinite(const glm::vec3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // Convert one world-axis coordinate to its integer cell index. Returns
        // false when the result is not representable as i32 — a finite but
        // enormous coordinate (|coord| / cellSize beyond ~2^31), or a value that
        // overflowed to +/-inf during the multiply. A float->int cast of an
        // out-of-range (or NaN/Inf) value is UB, so callers must skip the entity
        // (Insert) or fall back to a flat scan (queries) rather than cast.
        [[nodiscard]] bool TryWorldToCell(f32 world, f32 invCellSize, i32& outCell)
        {
            const f32 c = std::floor(world * invCellSize);
            // i32 range as reals is [-2^31, 2^31). The bounded form below is
            // also false for NaN/Inf, so it rejects every unsafe cast.
            if (!(c >= -2147483648.0f && c < 2147483648.0f))
            {
                return false;
            }
            outCell = static_cast<i32>(c);
            return true;
        }

        // True if walking the [min,max] cell box would visit strictly more cells
        // than there are entries. The cell walk costs O(box volume) regardless of
        // occupancy, so when the box dwarfs the entry count — a large query
        // radius over a fine grid, or a near-empty index — a direct entry scan
        // (O(entries)) is both faster and immune to a pathological empty-cell
        // explosion (a 1 km query on a 1 m grid is a billion empty lookups).
        // Overflow-safe: each partial product is bounded against `entryCount`
        // before the next multiply.
        [[nodiscard]] bool CellBoxLargerThanEntries(i32 minX, i32 maxX, i32 minY, i32 maxY,
                                                    i32 minZ, i32 maxZ, sizet entryCount)
        {
            const i64 limit = static_cast<i64>(entryCount);
            const i64 spanX = static_cast<i64>(maxX) - minX + 1;
            const i64 spanY = static_cast<i64>(maxY) - minY + 1;
            const i64 spanZ = static_cast<i64>(maxZ) - minZ + 1;
            if (spanX > limit)
            {
                return true;
            }
            i64 volume = spanX * spanY; // spanX <= limit, so this can't overflow i64
            if (volume > limit)
            {
                return true;
            }
            if (spanZ > limit)
            {
                return true;
            }
            volume *= spanZ; // volume <= limit and spanZ <= limit → fits i64
            return volume > limit;
        }
    } // namespace

    SceneSpatialIndex::SceneSpatialIndex(f32 cellSize)
    {
        // Reject non-positive AND non-finite sizes: a NaN slips past `<= 0`
        // (all NaN comparisons are false) and would make m_InvCellSize NaN,
        // poisoning every floor()/cast in the grid math (UB). Clamp to a safe
        // default instead.
        if (!(std::isfinite(cellSize) && cellSize > 0.0f))
        {
            OLO_CORE_WARN("SceneSpatialIndex: invalid cellSize {}, clamping to 1.0f", cellSize);
            cellSize = 1.0f;
        }
        m_CellSize = cellSize;
        m_InvCellSize = 1.0f / cellSize;
    }

    void SceneSpatialIndex::Clear()
    {
        // Drop all entries and all cell keys. m_Entries.clear() keeps the
        // vector's capacity, and unordered_map::clear() removes every (now-dead)
        // cell key while the implementation retains its bucket array — so the
        // next rebuild refills both without rehashing from scratch. Clearing
        // only the per-cell vectors (an earlier version) leaked the keys: as
        // moving entities visited new cells every tick, m_Cells grew without
        // bound with empty cells that were never reclaimed.
        m_Entries.clear();
        m_Cells.clear();
    }

    void SceneSpatialIndex::Insert(UUID id, const glm::vec3& position)
    {
        // A NaN/Inf position would make std::floor(pos * inv) produce a garbage
        // cell key (and never match any query), so drop it rather than poison
        // the grid. Mirrors the float-validation discipline used for any value
        // that could originate from physics/script divergence.
        if (!IsFinite(position))
        {
            return;
        }

        // Map to a cell via the checked conversion: a finite but enormous
        // position whose cell index overflows i32 is skipped rather than risking
        // a UB float->int cast. Such a position is unreachable by any well-formed
        // query anyway (queries near it would also overflow and fall back to the
        // entry scan, which won't contain a skipped entity — consistent).
        i32 cellX = 0;
        i32 cellY = 0;
        i32 cellZ = 0;
        if (!TryWorldToCell(position.x, m_InvCellSize, cellX) ||
            !TryWorldToCell(position.y, m_InvCellSize, cellY) ||
            !TryWorldToCell(position.z, m_InvCellSize, cellZ))
        {
            return;
        }

        const u32 index = static_cast<u32>(m_Entries.size());
        m_Entries.push_back(Entry{ id, position });
        m_Cells[CellKey{ cellX, cellY, cellZ }].push_back(index);
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

        // Reject the query before any cell math: a non-finite center or radius
        // would floor to a garbage integer cell range (potentially a near-
        // infinite loop), so screen it the same way Insert screens positions.
        if (radius < 0.0f || !std::isfinite(radius) || !IsFinite(center))
        {
            return;
        }

        const f32 radiusSq = radius * radius;

        // Only the cells overlapping the query sphere's bounding box can hold a
        // hit — visit those and distance-test each occupant exactly.
        i32 minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
        const bool representable =
            TryWorldToCell(center.x - radius, m_InvCellSize, minX) &&
            TryWorldToCell(center.x + radius, m_InvCellSize, maxX) &&
            TryWorldToCell(center.y - radius, m_InvCellSize, minY) &&
            TryWorldToCell(center.y + radius, m_InvCellSize, maxY) &&
            TryWorldToCell(center.z - radius, m_InvCellSize, minZ) &&
            TryWorldToCell(center.z + radius, m_InvCellSize, maxZ);

        // Flat entry scan when the cell box is unrepresentable (a huge finite
        // center/radius) or would span more cells than there are entries — both
        // make the cell walk either UB or pointlessly expensive.
        if (!representable ||
            CellBoxLargerThanEntries(minX, maxX, minY, maxY, minZ, maxZ, m_Entries.size()))
        {
            for (const Entry& entry : m_Entries)
            {
                if (glm::distance2(center, entry.Position) <= radiusSq)
                {
                    out.push_back(entry.Id);
                }
            }
            return;
        }

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

        if (!IsFinite(min) || !IsFinite(max))
        {
            return; // non-finite bounds → garbage cell range, screen it out
        }
        if (min.x > max.x || min.y > max.y || min.z > max.z)
        {
            return; // degenerate / inverted box holds nothing
        }

        i32 minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
        const bool representable =
            TryWorldToCell(min.x, m_InvCellSize, minX) &&
            TryWorldToCell(max.x, m_InvCellSize, maxX) &&
            TryWorldToCell(min.y, m_InvCellSize, minY) &&
            TryWorldToCell(max.y, m_InvCellSize, maxY) &&
            TryWorldToCell(min.z, m_InvCellSize, minZ) &&
            TryWorldToCell(max.z, m_InvCellSize, maxZ);

        const auto contains = [&](const glm::vec3& p)
        {
            return p.x >= min.x && p.x <= max.x &&
                   p.y >= min.y && p.y <= max.y &&
                   p.z >= min.z && p.z <= max.z;
        };

        // Same cells-vs-entries trade-off as QueryRadius, plus the unrepresentable
        // guard: a huge or far-flung box is answered by scanning the entries.
        if (!representable ||
            CellBoxLargerThanEntries(minX, maxX, minY, maxY, minZ, maxZ, m_Entries.size()))
        {
            for (const Entry& entry : m_Entries)
            {
                if (contains(entry.Position))
                {
                    out.push_back(entry.Id);
                }
            }
            return;
        }

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
                        if (contains(entry.Position))
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
        // A non-finite center or a NaN/negative maxRadius can't define a valid
        // search region — reject before any cell math or comparator runs.
        if (count == 0 || m_Entries.empty() || maxRadius < 0.0f ||
            std::isnan(maxRadius) || !IsFinite(center))
        {
            return result;
        }

        // (distance², UUID) candidate set. Bounded searches walk the cell range
        // around the sphere; an unbounded search would have to visit the whole
        // cell range anyway, so scan the flat entry list directly — same O(n)
        // cost without the cell-iteration overhead. "Unbounded" = the FLT_MAX
        // sentinel default OR a literal +inf radius (both mean "search all").
        std::vector<std::pair<f32, UUID>> candidates;

        const bool bounded = std::isfinite(maxRadius) &&
                             maxRadius != std::numeric_limits<f32>::max();
        if (bounded)
        {
            const f32 maxRadiusSq = maxRadius * maxRadius;

            i32 minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
            const bool representable =
                TryWorldToCell(center.x - maxRadius, m_InvCellSize, minX) &&
                TryWorldToCell(center.x + maxRadius, m_InvCellSize, maxX) &&
                TryWorldToCell(center.y - maxRadius, m_InvCellSize, minY) &&
                TryWorldToCell(center.y + maxRadius, m_InvCellSize, maxY) &&
                TryWorldToCell(center.z - maxRadius, m_InvCellSize, minZ) &&
                TryWorldToCell(center.z + maxRadius, m_InvCellSize, maxZ);

            if (!representable ||
                CellBoxLargerThanEntries(minX, maxX, minY, maxY, minZ, maxZ, m_Entries.size()))
            {
                // Box dwarfs the entry count → scan entries, still range-gated.
                for (const Entry& entry : m_Entries)
                {
                    const f32 distSq = glm::distance2(center, entry.Position);
                    if (distSq <= maxRadiusSq)
                    {
                        candidates.emplace_back(distSq, entry.Id);
                    }
                }
            }
            else
            {
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
        // Ignore non-positive / non-finite sizes (a NaN would poison the grid).
        if (!(std::isfinite(cellSize) && cellSize > 0.0f))
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
} // namespace OloEngine
