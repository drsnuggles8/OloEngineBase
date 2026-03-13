#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Fixed-size 2D spatial hash grid for O(1) entity lookups by position.
    // Entities are hashed by (x/cellSize, z/cellSize) on the XZ plane.
    class SpatialGrid
    {
      public:
        explicit SpatialGrid(f32 cellSize = 64.0f);

        // Insert or update an entity's position in the grid.
        void InsertOrUpdate(u64 uuid, const glm::vec3& position);

        // Remove an entity from the grid.
        void Remove(u64 uuid);

        // Clear all entities from the grid.
        void Clear();

        // Query all entities within a radius of the given center point.
        [[nodiscard]] std::vector<u64> QueryRadius(const glm::vec3& center, f32 radius) const;

        // Get the number of entities in the grid.
        [[nodiscard]] u32 GetEntityCount() const;

        // Get/set cell size.
        [[nodiscard]] f32 GetCellSize() const;
        void SetCellSize(f32 cellSize);

        // Get the number of entities in the cell containing the given position.
        [[nodiscard]] u32 GetCellPopulation(const glm::vec3& position) const;

      private:
        struct CellKey
        {
            i32 X = 0;
            i32 Z = 0;

            bool operator==(const CellKey& other) const
            {
                return X == other.X && Z == other.Z;
            }
        };

        struct CellKeyHash
        {
            std::size_t operator()(const CellKey& key) const
            {
                // Simple but effective spatial hash
                auto h1 = std::hash<i32>{}(key.X);
                auto h2 = std::hash<i32>{}(key.Z);
                return h1 ^ (h2 << 16) ^ (h2 >> 16);
            }
        };

        [[nodiscard]] CellKey PositionToCell(const glm::vec3& pos) const;

        f32 m_CellSize = 64.0f;
        f32 m_InvCellSize = 1.0f / 64.0f;

        // Cell → list of entity UUIDs in that cell
        std::unordered_map<CellKey, std::vector<u64>, CellKeyHash> m_Cells;

        // Entity → its current cell (for fast removal/update)
        std::unordered_map<u64, CellKey> m_EntityCells;

        // Entity → position (for distance checks during queries)
        std::unordered_map<u64, glm::vec3> m_EntityPositions;
    };
} // namespace OloEngine
