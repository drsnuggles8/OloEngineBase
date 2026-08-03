#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>

#include <cmath>
#include <span>
#include <type_traits>
#include <vector>

namespace OloEngine
{
    // =========================================================================
    // FlockSpatialHash — unbounded uniform-grid spatial hash over a flat array
    // of positions, addressed by INDEX.
    //
    // Why a FOURTH spatial structure exists (read this before adding a fifth):
    //
    //   * SceneSpatialIndex (Scene/SpatialAcceleration.h) is the general scene
    //     query index. It is keyed by UUID and indexes EVERY entity, so a boid
    //     neighbour query would return UUIDs that then need an entity-map
    //     lookup + a component fetch per neighbour, and would sweep every
    //     non-boid entity sharing a cell. It is also rebuilt by the game-thread
    //     "SpatialIndex" system, which is the wrong granularity (one global
    //     cell size) for a flock's perception radius.
    //   * CPUFluidSolver (Fluid/CPUFluidSolver.h) uses a DENSE grid with a
    //     head/next linked list. That is the right call for a fluid, whose
    //     domain is a bounded box; a flock roams an unbounded world, so a dense
    //     grid is not representable.
    //   * Networking/Replication/SpatialGrid.h is a fixed-size hash grid for
    //     replication interest management, keyed by UUID and sized to network
    //     relevance distance rather than to a perception radius.
    //
    // So this one is deliberately narrow: it stores nothing but positions and
    // hands back INDICES into the caller's own SoA arrays, which is what makes
    // the flocking inner loop a pair of contiguous array reads.
    //
    // Layout is CSR (counting sort), not a linked list: pass 1 counts per
    // bucket, a prefix sum gives the bucket offsets, pass 2 scatters item
    // indices in ascending index order. That keeps every bucket's members
    // contiguous, and — because the scatter is stable — makes the visit order
    // of a query a pure function of the input array's order. The fluid solver's
    // head/next list is equally deterministic but iterates each cell in reverse
    // insertion order and chases pointers; for a per-tick rebuild over a few
    // hundred agents the counting sort is both simpler to reason about and
    // friendlier to the cache.
    //
    // DETERMINISM CONTRACT (issue #731 acceptance criterion 2): for identical
    // inputs, Rebuild produces an identical structure and ForEachInRadius
    // visits neighbours in an identical order — cells in lexicographic
    // (z, y, x) order, and within a cell in ascending item index. No hash-map
    // iteration, no floating-point-keyed ordering, no allocation-address
    // dependence. Callers that accumulate floats over neighbours therefore get
    // bit-identical sums run to run, which is what lets the steering phase move
    // to a worker thread without changing results.
    //
    // NOT thread-safe: one instance is owned by one Scene and is rebuilt and
    // queried by the same system within a single scheduler node.
    // =========================================================================
    class FlockSpatialHash
    {
      public:
        // Cell sizes below this collapse the grid into pathologically many
        // cells for a normal world scale; a smaller request is clamped up.
        static constexpr f32 kMinCellSize = 0.01f;

        // Bucket count bounds (always a power of two, so the hash reduces with
        // a mask rather than a modulo).
        static constexpr u32 kMinBuckets = 16;
        static constexpr u32 kMaxBuckets = 1u << 20;

        // A query whose radius spans more than this many cells stops being a
        // win over a linear scan (and risks a pathological loop when a caller
        // passes a huge radius against a small cell size), so it falls back to
        // scanning every item. Both paths visit the same items in ascending
        // index order, so the fallback is a pure cost trade-off and is
        // indistinguishable to a caller — including one that stops the sweep
        // early, which would otherwise keep a different subset per path.
        static constexpr u32 kMaxQueryCells = 4096;

        // Rebuild the grid from scratch. `positions` is copied (indices in
        // callbacks refer to this array's ordering); a non-finite position is
        // simply left out of the grid rather than corrupting the cell maths,
        // so it can never be returned as a neighbour. Retains capacity across
        // calls — this is a per-tick rebuild on the hot path.
        void Rebuild(std::span<const glm::vec3> positions, f32 cellSize);

        // Drop every item, keeping capacity and the configured cell size.
        void Clear();

        // Visit every indexed item within `radius` of `center` (inclusive).
        //
        // `fn` is invoked as fn(u32 index, const glm::vec3& position, f32
        // distanceSq). It may return void, or return bool to control the sweep
        // — returning false stops the traversal early (used by the neighbour
        // cap in the steering kernel).
        //
        // Each item is visited at most once: an item belongs to exactly one
        // cell, every candidate is re-checked against the exact cell
        // coordinate (two distinct cells may share a bucket), and each cell
        // coordinate in range is visited exactly once.
        template<typename Fn>
        void ForEachInRadius(const glm::vec3& center, f32 radius, Fn&& fn) const;

        // Total items handed to the last Rebuild (including any that were
        // rejected as non-finite).
        [[nodiscard]] u32 GetItemCount() const
        {
            return static_cast<u32>(m_Positions.size());
        }

        // Items actually placed in the grid — GetItemCount() minus the
        // non-finite rejects. A silent divergence between the two is the
        // signal that some upstream system is producing NaN positions.
        [[nodiscard]] u32 GetIndexedItemCount() const
        {
            return m_IndexedCount;
        }

        [[nodiscard]] u32 GetBucketCount() const
        {
            return m_BucketCount;
        }

        // Buckets holding at least one item, and the largest bucket's load.
        // Occupancy / cell-size tuning signals: a max load approaching the
        // item count means the cell size is far too large (or every agent is
        // stacked in one spot) and queries have degenerated to linear scans.
        [[nodiscard]] u32 GetOccupiedBucketCount() const
        {
            return m_OccupiedBuckets;
        }

        [[nodiscard]] u32 GetMaxBucketLoad() const
        {
            return m_MaxBucketLoad;
        }

        [[nodiscard]] f32 GetCellSize() const
        {
            return m_CellSize;
        }

      private:
        struct CellCoord
        {
            i32 X = 0;
            i32 Y = 0;
            i32 Z = 0;

            [[nodiscard]] bool operator==(const CellCoord& other) const
            {
                return X == other.X && Y == other.Y && Z == other.Z;
            }
        };

        // Clamp before the cast: a finite-but-astronomical coordinate divided
        // by the cell size can still exceed i32's range, and the cast would be
        // UB. Clamping folds those agents into the extreme cells, which is
        // harmless (they are nowhere near anything).
        [[nodiscard]] static i32 AxisToCell(f32 value, f32 invCellSize)
        {
            constexpr f32 kLimit = 1.0e9f;
            const f32 scaled = std::floor(value * invCellSize);
            return static_cast<i32>(scaled < -kLimit ? -kLimit : (scaled > kLimit ? kLimit : scaled));
        }

        [[nodiscard]] CellCoord ToCell(const glm::vec3& position) const
        {
            return { AxisToCell(position.x, m_InvCellSize), AxisToCell(position.y, m_InvCellSize),
                     AxisToCell(position.z, m_InvCellSize) };
        }

        // The classic Teschner et al. triple-prime mix. Done in u32 so the
        // multiplication wraps rather than overflowing a signed type.
        [[nodiscard]] u32 BucketOf(i32 x, i32 y, i32 z) const
        {
            const u32 h = (static_cast<u32>(x) * 73856093u) ^ (static_cast<u32>(y) * 19349663u) ^
                          (static_cast<u32>(z) * 83492791u);
            return h & m_BucketMask;
        }

        [[nodiscard]] u32 BucketOf(const CellCoord& cell) const
        {
            return BucketOf(cell.X, cell.Y, cell.Z);
        }

        f32 m_CellSize = 1.0f;
        f32 m_InvCellSize = 1.0f;

        u32 m_BucketCount = 0;
        u32 m_BucketMask = 0;
        u32 m_IndexedCount = 0;
        u32 m_OccupiedBuckets = 0;
        u32 m_MaxBucketLoad = 0;

        // Copy of the caller's positions — queries do the exact distance test
        // here rather than calling back into the caller's arrays.
        std::vector<glm::vec3> m_Positions;
        // Cell coordinate per item, parallel to m_Positions. Meaningless for a
        // rejected (non-finite) item, which is never in m_Items.
        std::vector<CellCoord> m_Cells;
        // CSR: m_Items[m_BucketStart[b] .. m_BucketStart[b + 1]) are the item
        // indices in bucket b, ascending. Sized m_BucketCount + 1.
        std::vector<u32> m_BucketStart;
        std::vector<u32> m_Items;
        // Scatter cursors, kept as a member purely to avoid a per-tick alloc.
        std::vector<u32> m_Cursor;
    };

    template<typename Fn>
    void FlockSpatialHash::ForEachInRadius(const glm::vec3& center, f32 radius, Fn&& fn) const
    {
        // Invoke fn and report whether the sweep should continue, supporting
        // both the void and the bool-returning callback shapes.
        const auto visit = [&fn](u32 index, const glm::vec3& position, f32 distanceSq) -> bool
        {
            if constexpr (std::is_same_v<std::invoke_result_t<Fn&, u32, const glm::vec3&, f32>, bool>)
            {
                return fn(index, position, distanceSq);
            }
            else
            {
                fn(index, position, distanceSq);
                return true;
            }
        };

        if (m_IndexedCount == 0 || m_BucketCount == 0)
            return;
        if (!(radius >= 0.0f) || !std::isfinite(radius))
            return;
        if (!Math::IsFinite(center))
            return;

        const f32 radiusSq = radius * radius;

        const CellCoord lo = ToCell(center - glm::vec3(radius));
        const CellCoord hi = ToCell(center + glm::vec3(radius));

        // i64 so a span straddling the clamped extremes can't overflow.
        const i64 spanX = static_cast<i64>(hi.X) - static_cast<i64>(lo.X) + 1;
        const i64 spanY = static_cast<i64>(hi.Y) - static_cast<i64>(lo.Y) + 1;
        const i64 spanZ = static_cast<i64>(hi.Z) - static_cast<i64>(lo.Z) + 1;

        if (spanX * spanY * spanZ > static_cast<i64>(kMaxQueryCells))
        {
            // Degenerate query — scan every item instead.
            //
            // Walk m_Positions, NOT m_Items: the CSR item array is grouped by
            // bucket, so iterating it would visit in bucket-major order, which
            // is deterministic but is NOT the same order as the cell sweep
            // above. That difference is observable — a caller that stops early
            // (the steering kernel's neighbour cap) would keep a different
            // subset depending on which path ran — so the two paths must agree
            // on order, not merely on the set they can produce.
            const u32 itemCount = static_cast<u32>(m_Positions.size());
            for (u32 index = 0; index < itemCount; ++index)
            {
                const glm::vec3& position = m_Positions[index];
                // Skip the items Rebuild rejected, so the fallback returns
                // exactly what the grid holds. (The distance test below would
                // reject them anyway — every comparison against a NaN or
                // infinite delta is false — but relying on that is a subtlety
                // no reader should have to reconstruct.)
                if (!Math::IsFinite(position))
                    continue;
                const glm::vec3 delta = position - center;
                const f32 distanceSq = glm::dot(delta, delta);
                if (distanceSq <= radiusSq && !visit(index, position, distanceSq))
                    return;
            }
            return;
        }

        // Lexicographic (z, y, x) cell walk — the ordering half of the
        // determinism contract.
        for (i32 cz = lo.Z; cz <= hi.Z; ++cz)
        {
            for (i32 cy = lo.Y; cy <= hi.Y; ++cy)
            {
                for (i32 cx = lo.X; cx <= hi.X; ++cx)
                {
                    const CellCoord cell{ cx, cy, cz };
                    const u32 bucket = BucketOf(cell);
                    const u32 end = m_BucketStart[bucket + 1];
                    for (u32 slot = m_BucketStart[bucket]; slot < end; ++slot)
                    {
                        const u32 index = m_Items[slot];
                        // Two distinct cells can land in the same bucket; the
                        // exact-cell test is what keeps a query from returning
                        // a far-away collider AND from visiting an item twice.
                        if (!(m_Cells[index] == cell))
                            continue;

                        const glm::vec3& position = m_Positions[index];
                        const glm::vec3 delta = position - center;
                        const f32 distanceSq = glm::dot(delta, delta);
                        if (distanceSq > radiusSq)
                            continue;
                        if (!visit(index, position, distanceSq))
                            return;
                    }
                }
            }
        }
    }
} // namespace OloEngine
