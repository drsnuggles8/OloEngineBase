#include "OloEnginePCH.h"
#include "OloEngine/Renderer/AtlasAllocator.h"

#include "OloEngine/Memory/AlignmentTemplates.h"

#include <algorithm>

namespace OloEngine
{
    AtlasAllocator::AtlasAllocator(u32 atlasSize, u32 minTileSize)
    {
        if (atlasSize == 0 || minTileSize == 0 || minTileSize > atlasSize ||
            !IsPowerOfTwo(atlasSize) || !IsPowerOfTwo(minTileSize))
            return; // zero-capacity: every Allocate() fails, never asserts

        // Checked BEFORE touching m_AtlasSize/m_MinTileSize/m_LevelCount, and
        // before LevelStart() ever sees this value: past kMaxLevelCount its
        // `1u << (2*level)` shift is undefined behaviour, and well before
        // that the (4^level-1)/3-sized node table is an unreasonable
        // allocation for a ratio nothing here legitimately needs.
        const u32 levelCount = FloorLogTwo(atlasSize / minTileSize) + 1u;
        if (levelCount > kMaxLevelCount)
            return; // zero-capacity: same contract as any other invalid input

        m_AtlasSize = atlasSize;
        m_MinTileSize = minTileSize;
        m_LevelCount = levelCount;

        const u32 totalNodes = LevelStart(m_LevelCount);
        m_Nodes.assign(totalNodes, Node{});
        m_NodeLevel.resize(totalNodes);
        for (u32 level = 0; level < m_LevelCount; ++level)
        {
            const u32 start = LevelStart(level);
            const u32 end = LevelStart(level + 1u);
            std::fill(m_NodeLevel.begin() + start, m_NodeLevel.begin() + end, static_cast<u8>(level));
        }
    }

    u32 AtlasAllocator::LevelStart(u32 level)
    {
        // (4^level - 1) / 3 -- first node index at `level` in the 4-ary heap.
        return ((1u << (2u * level)) - 1u) / 3u;
    }

    u32 AtlasAllocator::ParentOf(u32 node)
    {
        return (node - 1u) / 4u;
    }

    u32 AtlasAllocator::LevelForSize(u32 size) const
    {
        if (m_AtlasSize == 0 || size == 0 || size < m_MinTileSize || size > m_AtlasSize || !IsPowerOfTwo(size))
            return kInvalidNode;
        return FloorLogTwo(m_AtlasSize / size);
    }

    bool AtlasAllocator::CanAllocate(u32 node) const
    {
        if (m_Nodes[node].Allocated || m_Nodes[node].RefCount > 0)
            return false; // this node (or a finer descendant of it) is taken

        for (u32 n = node; n != 0;)
        {
            n = ParentOf(n);
            if (m_Nodes[n].Allocated)
                return false; // a coarser ancestor already covers this area
        }
        return true;
    }

    void AtlasAllocator::MarkAncestors(u32 node, bool increment)
    {
        for (u32 n = node; n != 0;)
        {
            n = ParentOf(n);
            if (increment)
            {
                ++m_Nodes[n].RefCount;
            }
            else
            {
                OLO_CORE_ASSERT(m_Nodes[n].RefCount > 0, "AtlasAllocator: ancestor refcount underflow");
                --m_Nodes[n].RefCount;
            }
        }
    }

    u32 AtlasAllocator::Allocate(u32 size)
    {
        const u32 level = LevelForSize(size);
        if (level == kInvalidNode || level >= m_LevelCount)
            return kInvalidNode;

        const u32 start = LevelStart(level);
        const u32 end = LevelStart(level + 1u);
        for (u32 node = start; node < end; ++node)
        {
            if (!CanAllocate(node))
                continue;

            m_Nodes[node].Allocated = true;
            MarkAncestors(node, /*increment*/ true);
            ++m_LiveCount;
            return node;
        }
        return kInvalidNode;
    }

    bool AtlasAllocator::Free(u32 node)
    {
        if (node == kInvalidNode || node >= m_Nodes.size() || !m_Nodes[node].Allocated)
            return false;

        m_Nodes[node].Allocated = false;
        MarkAncestors(node, /*increment*/ false);
        --m_LiveCount;
        return true;
    }

    void AtlasAllocator::Reset()
    {
        std::fill(m_Nodes.begin(), m_Nodes.end(), Node{});
        m_LiveCount = 0;
    }

    bool AtlasAllocator::IsAllocated(u32 node) const
    {
        return node < m_Nodes.size() && m_Nodes[node].Allocated;
    }

    AtlasAllocator::Region AtlasAllocator::GetRegion(u32 node) const
    {
        if (node >= m_Nodes.size())
            return {};

        const u32 level = m_NodeLevel[node];
        const u32 localIndex = node - LevelStart(level);
        const u32 size = m_AtlasSize >> level;

        u32 x = 0, y = 0;
        u32 cellSize = m_AtlasSize;
        for (u32 depth = 0; depth < level; ++depth)
        {
            cellSize >>= 1u;
            const u32 shift = 2u * (level - 1u - depth);
            const u32 quadrant = (localIndex >> shift) & 3u;
            if (quadrant & 1u)
                x += cellSize;
            if (quadrant & 2u)
                y += cellSize;
        }
        return { x, y, size };
    }

    f32 AtlasAllocator::Occupancy() const
    {
        if (m_AtlasSize == 0)
            return 0.0f;

        u64 allocatedArea = 0;
        for (u32 level = 0; level < m_LevelCount; ++level)
        {
            const u32 size = m_AtlasSize >> level;
            const u64 area = static_cast<u64>(size) * static_cast<u64>(size);
            const u32 start = LevelStart(level);
            const u32 end = LevelStart(level + 1u);
            for (u32 node = start; node < end; ++node)
            {
                if (m_Nodes[node].Allocated)
                    allocatedArea += area;
            }
        }

        const u64 totalArea = static_cast<u64>(m_AtlasSize) * static_cast<u64>(m_AtlasSize);
        return static_cast<f32>(static_cast<f64>(allocatedArea) / static_cast<f64>(totalArea));
    }
} // namespace OloEngine
