#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <limits>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // General-purpose spatial acceleration structure for dynamic scene queries.
    //
    // A uniform spatial hash grid over entity positions, keyed by UUID. It is
    // rebuilt once per tick from the live TransformComponent positions (see
    // Scene::OnUpdateRuntime) and then consumed by gameplay systems that would
    // otherwise scan every candidate entity — e.g. AI sight perception, which
    // used to do an O(n) linear scan over all perceptible entities per perceiver.
    //
    // This first slice is a uniform grid (simplest correct structure); a BVH /
    // octree is a natural follow-up if scenes with widely-varying entity density
    // need it (issue #430). The query surface (QueryRadius / QueryAABB / NearestN)
    // is structure-agnostic, so a later swap to a hierarchical index need not
    // touch callers.
    //
    // NOT thread-safe: build and query happen on the game thread inside the
    // single-threaded Scene tick. Positions are the entity's TransformComponent
    // Translation (local translation treated as world, matching the consumers
    // that read .Translation directly).
    class SceneSpatialIndex
    {
      public:
        explicit SceneSpatialIndex(f32 cellSize = 10.0f);

        // Drop every entry, keeping the configured cell size. Called at the
        // start of each per-tick rebuild.
        void Clear();

        // Add one entity at `position`. A non-finite position is rejected
        // (skipped) so a NaN/Inf transform — e.g. a physics body that blew up —
        // can never corrupt the cell hashing. Duplicate UUIDs are not
        // de-duplicated: the rebuild path inserts each live entity exactly once.
        void Insert(UUID id, const glm::vec3& position);

        // All entities whose position lies within `radius` of `center`
        // (inclusive). Order is unspecified. A negative radius yields nothing.
        [[nodiscard]] std::vector<UUID> QueryRadius(const glm::vec3& center, f32 radius) const;

        // Append form of QueryRadius — results are pushed onto `out` (not
        // cleared first), so a caller can reuse one scratch buffer across many
        // queries without per-call allocation (the AI perception hot path does
        // exactly this).
        void QueryRadius(const glm::vec3& center, f32 radius, std::vector<UUID>& out) const;

        // All entities whose position lies inside the axis-aligned box
        // [min, max] (inclusive on every axis). Order is unspecified. If any
        // min component exceeds the matching max, the box is empty.
        [[nodiscard]] std::vector<UUID> QueryAABB(const glm::vec3& min, const glm::vec3& max) const;
        void QueryAABB(const glm::vec3& min, const glm::vec3& max, std::vector<UUID>& out) const;

        // The up-to-`count` entities nearest `center`, sorted nearest-first.
        // Only entities within `maxRadius` are considered; pass the default
        // sentinel to search the whole index (an O(n) scan — bound it when the
        // index is large). Ties break arbitrarily but deterministically for a
        // fixed insertion order.
        [[nodiscard]] std::vector<UUID> NearestN(const glm::vec3& center, u32 count,
                                                 f32 maxRadius = std::numeric_limits<f32>::max()) const;

        [[nodiscard]] u32 GetEntityCount() const
        {
            return static_cast<u32>(m_Entries.size());
        }

        // Number of distinct occupied cells. Primarily an introspection /
        // test hook: a correct rebuild holds at most one cell per entity, so
        // this never exceeds GetEntityCount(). It catches a Clear() that frees
        // the per-cell lists but leaks their (now-empty) keys — m_Cells would
        // then grow without bound across rebuilds while GetEntityCount() stays
        // flat. Also useful as a coarse occupancy / cell-size tuning signal.
        [[nodiscard]] u32 GetCellCount() const
        {
            return static_cast<u32>(m_Cells.size());
        }

        [[nodiscard]] f32 GetCellSize() const
        {
            return m_CellSize;
        }

        // Change the grid resolution and re-hash existing entries. A
        // non-positive size is ignored. Cell size ~= the typical query radius
        // keeps each query touching a small constant number of cells.
        void SetCellSize(f32 cellSize);

      private:
        struct CellKey
        {
            i32 X = 0;
            i32 Y = 0;
            i32 Z = 0;

            bool operator==(const CellKey& other) const
            {
                return X == other.X && Y == other.Y && Z == other.Z;
            }
        };

        struct CellKeyHash
        {
            std::size_t operator()(const CellKey& key) const
            {
                // Same mixing as Networking/Replication/SpatialGrid — spreads
                // the three axes across the hash so neighbouring cells don't
                // collide into the same bucket.
                auto h1 = std::hash<i32>{}(key.X);
                auto h2 = std::hash<i32>{}(key.Y);
                auto h3 = std::hash<i32>{}(key.Z);
                return h1 ^ (h2 << 11) ^ (h3 << 22) ^ (h2 >> 21) ^ (h3 >> 10);
            }
        };

        struct Entry
        {
            UUID Id;
            glm::vec3 Position;
        };

        f32 m_CellSize = 10.0f;
        f32 m_InvCellSize = 1.0f / 10.0f;

        // Flat list of every indexed entity; cells store indices into this.
        // Keeping positions here (rather than in the cell vectors) lets queries
        // do the exact distance test without a second lookup.
        std::vector<Entry> m_Entries;

        // Cell coordinate -> indices into m_Entries occupying that cell.
        std::unordered_map<CellKey, std::vector<u32>, CellKeyHash> m_Cells;
    };
} // namespace OloEngine
