#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/ChunkRingBuffer3D.h"

#include <glm/glm.hpp>

#include <cmath>
#include <functional>
#include <utility>

namespace OloEngine
{
    // A fixed cubic window of chunks streamed around a tracked position (#729).
    //
    // Built for a volumetric/voxel world, where the loaded set is a 3D
    // neighbourhood rather than the 2D quadtree window TerrainStreamer uses for
    // heightmap terrain (see docs/agent-rules/terrain-gpu-lod-quadtree.md — that
    // is a different subsystem and this does not replace it). Backed by
    // ChunkRingBuffer3D for O(1), non-hashing lookup.
    //
    // Rebase invariance: feed Update() a position in the SAME frame the window
    // was built against — an anchor-relative position (trackedPosition minus
    // some entity/anchor translation that itself shifts under
    // Scene::RebaseOrigin) rather than the raw absolute world position. Both
    // terms then shift by the same delta and cancel, so the window never even
    // observes a rebase — the same fix documented for terrain streaming in
    // docs/agent-rules/floating-origin-rebase-subsystems.md §4. Feeding an
    // absolute or render-relative position that itself jumps across a rebase
    // will make the window treat the rebase as a teleport (see Update()).
    template<typename T>
    class VolumetricChunkWindow
    {
      public:
        // Returns the chunk's data, constructed at the given chunk coordinate.
        using LoadFn = std::function<T(const glm::ivec3&)>;
        // Called for a chunk leaving the window, before its slot is reused.
        using UnloadFn = std::function<void(const glm::ivec3&, T&)>;

        struct Config
        {
            f32 ChunkWorldSize = 32.0f;
            // Window covers chunk coordinates within LoadRadius of the centre
            // on every axis — side length 2*LoadRadius+1.
            u32 LoadRadius = 3;
            // Chunks within RenderRadius of the centre are surfaced by
            // ForEachRenderableChunk; RenderRadius <= LoadRadius lets the
            // loaded set stay ahead of what's actually drawn (loaded and
            // rendered radii configurable independently, per the issue).
            u32 RenderRadius = 3;
        };

        // (INT32_MAX - 1) / 2: the largest LoadRadius for which 2*LoadRadius+1
        // (the ring buffer's side length) still fits in an i32 — Update()'s
        // teleport check casts the side length to i32. A LoadRadius above this
        // would also ask ChunkRingBuffer3D to allocate side^3 elements, which
        // fails long before the cast does.
        static constexpr u32 kMaxLoadRadius = 1073741823u;

        VolumetricChunkWindow(const Config& config, LoadFn loadFn, UnloadFn unloadFn = nullptr)
            : m_Config(config), m_Load(std::move(loadFn)), m_Unload(std::move(unloadFn)),
              m_Buffer(ValidatedSideLength(config.LoadRadius))
        {
            OLO_CORE_ASSERT(m_Load, "VolumetricChunkWindow requires a load callback");
            OLO_CORE_ASSERT(config.RenderRadius <= config.LoadRadius,
                            "RenderRadius must not exceed LoadRadius");
        }

        // Convert a world-space position to the chunk coordinate containing it.
        // Floor division (not truncation) so negative coordinates land in the
        // chunk on the correct side of the origin.
        [[nodiscard]] static glm::ivec3 WorldToChunk(const glm::vec3& worldPos, f32 chunkWorldSize)
        {
            OLO_CORE_ASSERT(chunkWorldSize > 0.0f && std::isfinite(chunkWorldSize),
                            "WorldToChunk requires a positive, finite chunkWorldSize");
            OLO_CORE_ASSERT(std::isfinite(worldPos.x) && std::isfinite(worldPos.y) && std::isfinite(worldPos.z),
                            "WorldToChunk requires a finite worldPos");

            return glm::ivec3(static_cast<i32>(glm::floor(worldPos.x / chunkWorldSize)),
                              static_cast<i32>(glm::floor(worldPos.y / chunkWorldSize)),
                              static_cast<i32>(glm::floor(worldPos.z / chunkWorldSize)));
        }

        // Call once per frame/tick with the tracked position (camera, player,
        // …) in the window's reference frame — see the rebase note above.
        //
        // Returns true if the window's centre chunk changed. A move of N
        // chunks on an axis loads exactly the N newly-covered planes on that
        // axis and drops the N vacated ones — never a full-volume rescan —
        // UNLESS the move exceeds the window's own diameter (a teleport, where
        // nothing in the old window overlaps the new one), in which case the
        // whole window is rebuilt because there is no incoming plane to speak
        // of: every slot is incoming.
        bool Update(const glm::vec3& trackedPosition)
        {
            const glm::ivec3 newCentre = WorldToChunk(trackedPosition, m_Config.ChunkWorldSize);

            if (!m_Initialized)
            {
                m_CentreChunk = newCentre;
                m_Initialized = true;
                Rebuild();
                return true;
            }

            if (newCentre == m_CentreChunk)
            {
                return false;
            }

            const glm::ivec3 diff = newCentre - m_CentreChunk;
            const i32 diameter = static_cast<i32>(m_Buffer.GetSideLength());

            if (glm::abs(diff.x) >= diameter || glm::abs(diff.y) >= diameter || glm::abs(diff.z) >= diameter)
            {
                m_CentreChunk = newCentre;
                Rebuild();
                return true;
            }

            // Walk the move one chunk at a time per axis so each step loads
            // exactly the plane it uncovers, whether the caller moved one
            // chunk or several in a single tick.
            for (i32 axis = 0; axis < 3; ++axis)
            {
                const i32 step = diff[axis] > 0 ? 1 : -1;
                for (i32 moved = 0; moved != diff[axis]; moved += step)
                {
                    m_CentreChunk[axis] += step;
                    LoadIncomingPlane(axis, step);
                }
            }

            return true;
        }

        // O(1), no hashing. Only valid for coordinates within LoadRadius of the
        // current centre on every axis; returns nullptr outside that window.
        [[nodiscard]] const T* TryGetChunk(const glm::ivec3& chunkCoord) const
        {
            const Slot& slot = m_Buffer.At(chunkCoord);
            return (slot.Loaded && slot.Coord == chunkCoord) ? &slot.Data : nullptr;
        }
        [[nodiscard]] T* TryGetChunk(const glm::ivec3& chunkCoord)
        {
            Slot& slot = m_Buffer.At(chunkCoord);
            return (slot.Loaded && slot.Coord == chunkCoord) ? &slot.Data : nullptr;
        }

        // Visits every loaded chunk within RenderRadius of the current centre.
        // Fn: void(const glm::ivec3& coord, const T& data)
        template<typename Fn>
        void ForEachRenderableChunk(Fn&& fn) const
        {
            const i32 r = static_cast<i32>(m_Config.RenderRadius);
            for (i32 y = -r; y <= r; ++y)
            {
                for (i32 z = -r; z <= r; ++z)
                {
                    for (i32 x = -r; x <= r; ++x)
                    {
                        const glm::ivec3 coord = m_CentreChunk + glm::ivec3(x, y, z);
                        const Slot& slot = m_Buffer.At(coord);
                        if (slot.Loaded && slot.Coord == coord)
                        {
                            fn(coord, slot.Data);
                        }
                    }
                }
            }
        }

        [[nodiscard]] const glm::ivec3& GetCentreChunk() const
        {
            return m_CentreChunk;
        }
        [[nodiscard]] u32 GetLoadRadius() const
        {
            return m_Config.LoadRadius;
        }
        [[nodiscard]] u32 GetRenderRadius() const
        {
            return m_Config.RenderRadius;
        }

        // Reloads every chunk in the window at the current centre. Used
        // internally for the first Update() and for a teleport; exposed so a
        // caller can force a reload (e.g. the load callback's data source
        // changed underneath it).
        void Rebuild()
        {
            const i32 r = static_cast<i32>(m_Config.LoadRadius);
            for (i32 y = -r; y <= r; ++y)
            {
                for (i32 z = -r; z <= r; ++z)
                {
                    for (i32 x = -r; x <= r; ++x)
                    {
                        const glm::ivec3 coord = m_CentreChunk + glm::ivec3(x, y, z);
                        LoadSlot(coord);
                    }
                }
            }
        }

      private:
        struct Slot
        {
            bool Loaded = false;
            glm::ivec3 Coord{ 0 };
            T Data{};
        };

        // Validates LoadRadius BEFORE the ring buffer's side length is computed
        // or handed to ChunkRingBuffer3D — that constructor allocates side^3
        // elements, so an unvalidated overflow here would either wrap into a
        // tiny, wrong-sized buffer or attempt a catastrophic allocation.
        [[nodiscard]] static u32 ValidatedSideLength(u32 loadRadius)
        {
            OLO_CORE_ASSERT(loadRadius <= kMaxLoadRadius,
                            "LoadRadius exceeds the maximum safe value (2*LoadRadius+1 must fit in i32)");
            return 2 * loadRadius + 1;
        }

        void LoadSlot(const glm::ivec3& coord)
        {
            Slot& slot = m_Buffer.At(coord);
            if (slot.Loaded && m_Unload)
            {
                m_Unload(slot.Coord, slot.Data);
            }
            slot.Data = m_Load(coord);
            slot.Coord = coord;
            slot.Loaded = true;
        }

        // Loads the single plane of chunks newly covered after the centre has
        // just moved by one chunk along `axis` in direction `step` (+1/-1).
        // The vacated plane on the opposite face is exactly what LoadSlot's
        // wraparound overwrite evicts — the ring buffer's toroidal storage
        // means that plane's old slots ARE the new plane's slots.
        void LoadIncomingPlane(i32 axis, i32 step)
        {
            const i32 r = static_cast<i32>(m_Config.LoadRadius);
            const i32 planeOffset = step > 0 ? r : -r;

            i32 axisA = (axis + 1) % 3;
            i32 axisB = (axis + 2) % 3;

            for (i32 a = -r; a <= r; ++a)
            {
                for (i32 b = -r; b <= r; ++b)
                {
                    glm::ivec3 local(0);
                    local[axis] = planeOffset;
                    local[axisA] = a;
                    local[axisB] = b;

                    LoadSlot(m_CentreChunk + local);
                }
            }
        }

        Config m_Config;
        LoadFn m_Load;
        UnloadFn m_Unload;
        ChunkRingBuffer3D<Slot> m_Buffer;
        glm::ivec3 m_CentreChunk{ 0 };
        bool m_Initialized = false;
    };
} // namespace OloEngine
