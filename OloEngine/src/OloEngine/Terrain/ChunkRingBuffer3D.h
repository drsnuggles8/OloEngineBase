#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"

#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    // Fixed-size cubic toroidal ring buffer, addressed by a signed 3D chunk
    // coordinate that may be arbitrarily large or negative. Storage coordinate
    // `s` on an axis holds whichever chunk coordinate is currently congruent to
    // `s` modulo the side length — so moving the logical window by one chunk
    // does not move any of the other chunks' storage slots, it just relabels
    // which coordinate the wrapped-around slot now represents.
    //
    // O(1) lookup, no hashing, no per-lookup allocation: `At()` is one modulo
    // per axis. The caller (VolumetricChunkWindow) is responsible for keeping
    // track of which coordinates are currently valid within the window — this
    // container is a flat array with wraparound indexing and nothing more.
    template<typename T>
    class ChunkRingBuffer3D
    {
      public:
        explicit ChunkRingBuffer3D(u32 sideLength)
            : m_SideLength(sideLength), m_Storage(static_cast<sizet>(sideLength) * sideLength * sideLength)
        {
            OLO_CORE_ASSERT(sideLength % 2 == 1,
                            "ChunkRingBuffer3D side length must be odd to centre a window on (0,0,0)");
            OLO_CORE_ASSERT(sideLength > 0, "ChunkRingBuffer3D side length must be positive");
        }

        [[nodiscard]] T& At(const glm::ivec3& chunkCoord)
        {
            return m_Storage[Index(chunkCoord)];
        }
        [[nodiscard]] const T& At(const glm::ivec3& chunkCoord) const
        {
            return m_Storage[Index(chunkCoord)];
        }

        [[nodiscard]] u32 GetSideLength() const
        {
            return m_SideLength;
        }

      private:
        [[nodiscard]] sizet Index(const glm::ivec3& chunkCoord) const
        {
            // Euclidean modulo (never negative), so a negative chunk
            // coordinate wraps into the same slot cycle as its positive
            // congruents instead of producing a negative array index — the
            // same footgun and the same fix as DDGI's cascade addressing
            // (see Math::WrapIndex's doc comment).
            const i32 length = static_cast<i32>(m_SideLength);
            const i32 x = Math::WrapIndex(chunkCoord.x, length);
            const i32 y = Math::WrapIndex(chunkCoord.y, length);
            const i32 z = Math::WrapIndex(chunkCoord.z, length);
            return static_cast<sizet>(x) + static_cast<sizet>(z) * length +
                   static_cast<sizet>(y) * length * length;
        }

        u32 m_SideLength;
        std::vector<T> m_Storage;
    };
} // namespace OloEngine
