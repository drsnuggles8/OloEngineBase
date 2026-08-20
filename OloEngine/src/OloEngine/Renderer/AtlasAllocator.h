#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>

namespace OloEngine
{
    // @brief Reusable power-of-two square-region atlas allocator (issue #718).
    //
    // Allocates and frees axis-aligned SQUARE regions of size 2^k within a
    // power-of-two square atlas. The whole quadtree down to MinTileSize() is
    // preallocated up front (a fixed "4-ary heap": node 0 is the root/whole
    // atlas, the 4 children of node i live at 4i+1..4i+4), so freeing every
    // child of a node makes that node's own (coarser) region allocatable
    // again with no explicit merge step — the coalescing the issue asks for
    // falls out of the data structure rather than needing its own code path.
    //
    // Modelled on the MIT-licensed AtlasAllocatorPD2 (Hollow-TerrainSystem,
    // PhotoTerrain/Runtime/Utility/AtlasAllocatorPD2.cs), minus its Morton-code
    // / BMI2 fast path: that optimisation exists to skip whole allocated
    // subtrees in O(1) so a scan stays cheap however often Allocate() runs.
    // Both current instantiations call Allocate() only on a rare, non-hot-path
    // event (a shadow-atlas repack when a caster's rank/tier changes; an
    // impostor bake when a foliage layer's mesh/params change) rather than
    // every frame, so the plain O(nodes-at-that-level) linear scan this class
    // uses instead is cheap in practice even for the larger of the two node
    // tables (the impostor VRAM budget's 8192px/32px-min-tile allocator has
    // ~87k total nodes, ~65k of them at the finest level alone) — but it is
    // NOT "low thousands" across the board, and a future instantiation with
    // a genuinely hot Allocate() path and a large/fine-grained table should
    // measure before assuming this scan is still free.
    // Coordinate math stays free of platform intrinsics either way.
    class AtlasAllocator
    {
      public:
        static constexpr u32 kInvalidNode = 0xFFFFFFFFu;

        struct Region
        {
            u32 X = 0;
            u32 Y = 0;
            u32 Size = 0;
        };

        AtlasAllocator() = default;

        // atlasSize and minTileSize must both be powers of two, with
        // minTileSize in [1, atlasSize]. An invalid combination (zero, not a
        // power of two, minTileSize > atlasSize) yields a zero-capacity
        // allocator — every Allocate() call then returns kInvalidNode rather
        // than asserting, so a caller threading through a user/asset-controlled
        // resolution stays crash-free.
        AtlasAllocator(u32 atlasSize, u32 minTileSize);

        // Allocates a square region of exactly `size` (must be a power of two
        // in [MinTileSize(), AtlasSize()]). Returns kInvalidNode when no
        // region of that size is currently free (an out-of-range or non-power-
        // of-two size also fails this way, never asserts).
        [[nodiscard]] u32 Allocate(u32 size);

        // Releases a previously-allocated node, making its region (and,
        // transitively, any coarser ancestor whose other children are also
        // now free) available again. Freeing kInvalidNode or a node that is
        // not currently allocated is a no-op that returns false — mirrors the
        // reference allocator's tolerant Free().
        bool Free(u32 node);

        // Drops every live allocation, returning to the freshly-constructed
        // state without reallocating the node table.
        void Reset();

        [[nodiscard]] Region GetRegion(u32 node) const;
        [[nodiscard]] bool IsAllocated(u32 node) const;

        [[nodiscard]] u32 AtlasSize() const
        {
            return m_AtlasSize;
        }
        [[nodiscard]] u32 MinTileSize() const
        {
            return m_MinTileSize;
        }

        // Fraction (0..1) of the atlas AREA currently held by live
        // allocations. 0 for a default-constructed / zero-capacity allocator.
        [[nodiscard]] f32 Occupancy() const;

        // Count of currently-live (allocated) nodes.
        [[nodiscard]] u32 LiveAllocationCount() const
        {
            return m_LiveCount;
        }

      private:
        struct Node
        {
            u32 RefCount = 0; // number of allocated descendants (any depth)
            bool Allocated = false;
        };

        [[nodiscard]] u32 LevelForSize(u32 size) const;
        [[nodiscard]] static u32 LevelStart(u32 level);
        [[nodiscard]] static u32 ParentOf(u32 node);
        [[nodiscard]] bool CanAllocate(u32 node) const;
        void MarkAncestors(u32 node, bool increment);

        u32 m_AtlasSize = 0;
        u32 m_MinTileSize = 0;
        u32 m_LevelCount = 0; // number of levels; root is level 0
        std::vector<Node> m_Nodes;
        std::vector<u8> m_NodeLevel; // level of each node, parallel to m_Nodes
        u32 m_LiveCount = 0;
    };
} // namespace OloEngine
