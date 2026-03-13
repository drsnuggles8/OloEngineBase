#include "OloEnginePCH.h"
#include "SpatialGrid.h"
#include "OloEngine/Debug/Profiler.h"

#include <glm/gtx/norm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    SpatialGrid::SpatialGrid(f32 cellSize)
        : m_CellSize(cellSize)
        , m_InvCellSize(1.0f / cellSize)
    {
    }

    void SpatialGrid::InsertOrUpdate(u64 uuid, const glm::vec3& position)
    {
        CellKey const newCell = PositionToCell(position);
        m_EntityPositions[uuid] = position;

        if (auto it = m_EntityCells.find(uuid); it != m_EntityCells.end())
        {
            if (it->second == newCell)
            {
                return; // Same cell, just update position
            }

            // Remove from old cell
            auto& oldCellEntities = m_Cells[it->second];
            std::erase(oldCellEntities, uuid);
            if (oldCellEntities.empty())
            {
                m_Cells.erase(it->second);
            }
        }

        // Insert into new cell
        m_Cells[newCell].push_back(uuid);
        m_EntityCells[uuid] = newCell;
    }

    void SpatialGrid::Remove(u64 uuid)
    {
        auto it = m_EntityCells.find(uuid);
        if (it == m_EntityCells.end())
        {
            return;
        }

        auto& cellEntities = m_Cells[it->second];
        std::erase(cellEntities, uuid);
        if (cellEntities.empty())
        {
            m_Cells.erase(it->second);
        }

        m_EntityCells.erase(it);
        m_EntityPositions.erase(uuid);
    }

    void SpatialGrid::Clear()
    {
        m_Cells.clear();
        m_EntityCells.clear();
        m_EntityPositions.clear();
    }

    std::vector<u64> SpatialGrid::QueryRadius(const glm::vec3& center, f32 radius) const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u64> result;
        f32 const radiusSq = radius * radius;

        // Determine the range of cells that could contain entities within radius
        i32 const minX = static_cast<i32>(std::floor((center.x - radius) * m_InvCellSize));
        i32 const maxX = static_cast<i32>(std::floor((center.x + radius) * m_InvCellSize));
        i32 const minZ = static_cast<i32>(std::floor((center.z - radius) * m_InvCellSize));
        i32 const maxZ = static_cast<i32>(std::floor((center.z + radius) * m_InvCellSize));

        for (i32 x = minX; x <= maxX; ++x)
        {
            for (i32 z = minZ; z <= maxZ; ++z)
            {
                CellKey const key{ x, z };
                auto cellIt = m_Cells.find(key);
                if (cellIt == m_Cells.end())
                {
                    continue;
                }

                for (u64 uuid : cellIt->second)
                {
                    auto posIt = m_EntityPositions.find(uuid);
                    if (posIt == m_EntityPositions.end())
                    {
                        continue;
                    }

                    if (glm::distance2(center, posIt->second) <= radiusSq)
                    {
                        result.push_back(uuid);
                    }
                }
            }
        }

        return result;
    }

    u32 SpatialGrid::GetEntityCount() const
    {
        return static_cast<u32>(m_EntityCells.size());
    }

    f32 SpatialGrid::GetCellSize() const
    {
        return m_CellSize;
    }

    void SpatialGrid::SetCellSize(f32 cellSize)
    {
        if (cellSize <= 0.0f)
        {
            return;
        }
        m_CellSize = cellSize;
        m_InvCellSize = 1.0f / cellSize;
        // Note: existing entities are NOT re-hashed. Call Clear() + re-insert if cell size changes at runtime.
    }

    u32 SpatialGrid::GetCellPopulation(const glm::vec3& position) const
    {
        CellKey const key = PositionToCell(position);
        auto it = m_Cells.find(key);
        if (it == m_Cells.end())
        {
            return 0;
        }
        return static_cast<u32>(it->second.size());
    }

    SpatialGrid::CellKey SpatialGrid::PositionToCell(const glm::vec3& pos) const
    {
        return { static_cast<i32>(std::floor(pos.x * m_InvCellSize)),
                 static_cast<i32>(std::floor(pos.z * m_InvCellSize)) };
    }
} // namespace OloEngine
