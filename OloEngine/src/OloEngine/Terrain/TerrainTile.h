#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <glm/glm.hpp>
#include <string>
#include <atomic>

namespace OloEngine
{
    // Represents one independently loadable square tile of a large terrain.
    // Each tile has its own heightmap data, chunk manager, and material.
    // Tiles overlap by 1px at edges for seamless stitching.
    class TerrainTile : public RefCounted
    {
      public:
        enum class State : u8
        {
            Unloaded = 0,
            Loading,  // Async loading in progress
            Loaded,   // CPU data ready, needs GPU upload
            Ready,    // Fully built and renderable
            Unloading // Scheduled for cleanup
        };

        TerrainTile() = default;
        ~TerrainTile() override = default;

        // Grid position in the tile grid (not world coordinates)
        i32 GridX = 0;
        i32 GridZ = 0;

        // Tile resolution in height samples (e.g., 513 for 512 quads + 1-pixel overlap)
        u32 TileResolution = 513;

        // World-space dimensions of this tile
        f32 WorldSizeX = 256.0f;
        f32 WorldSizeZ = 256.0f;
        f32 HeightScale = 64.0f;

        // World origin of this tile
        glm::vec3 WorldOrigin{ 0.0f };

        // Load heightmap data from file (CPU-side, thread-safe for async use)
        bool LoadFromFile(const std::string& heightmapPath);

        // Create flat heightmap of given resolution
        void CreateFlat(u32 resolution, f32 defaultHeight = 0.0f);

        // Build GPU resources (chunks, quadtree) — must be called on main/render thread
        void BuildGPUResources(bool tessellationEnabled, f32 targetTriangleSize, f32 morphRegion);

        // Release all resources
        void Unload();

        // Accessors for rendering
        [[nodiscard]] Ref<TerrainData> GetTerrainData() const
        {
            return m_TerrainData;
        }
        [[nodiscard]] Ref<TerrainChunkManager> GetChunkManager() const
        {
            return m_ChunkManager;
        }
        [[nodiscard]] Ref<TerrainMaterial> GetMaterial() const
        {
            return m_Material;
        }

        void SetMaterial(const Ref<TerrainMaterial>& material)
        {
            m_Material = material;
        }

        [[nodiscard]] State GetState() const
        {
            return m_State.load(std::memory_order_acquire);
        }
        void SetState(State state)
        {
            m_State.store(state, std::memory_order_release);
        }

        // LRU timestamp for cache eviction (frame number when last used)
        u64 LastUsedFrame = 0;

        // Stitch edge heights with a neighbor tile. Direction: 0=+X, 1=-X, 2=+Z, 3=-Z
        void StitchEdge(const TerrainTile& neighbor, u32 direction);

      private:
        Ref<TerrainData> m_TerrainData;
        Ref<TerrainChunkManager> m_ChunkManager;
        Ref<TerrainMaterial> m_Material;

        std::atomic<State> m_State{ State::Unloaded };
    };
} // namespace OloEngine
