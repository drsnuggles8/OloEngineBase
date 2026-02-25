#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Terrain/TerrainTile.h"
#include "OloEngine/Task/Task.h"

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace OloEngine
{
    class Frustum;
    class TerrainMaterial;

    // Key for the tile grid (hash-friendly pair of ints)
    struct TileCoord
    {
        i32 X = 0;
        i32 Z = 0;

        bool operator==(const TileCoord& other) const { return X == other.X && Z == other.Z; }
    };

    struct TileCoordHash
    {
        sizet operator()(const TileCoord& c) const
        {
            sizet h1 = std::hash<i32>{}(c.X);
            sizet h2 = std::hash<i32>{}(c.Z);
            return h1 ^ (h2 * 0x9E3779B97F4A7C15ULL + 0x9E3779B9ULL + (h1 << 6) + (h1 >> 2));
        }
    };

    // Configuration for the terrain streamer
    struct TerrainStreamerConfig
    {
        // World-space size of each tile (shared for all tiles)
        f32 TileWorldSize = 256.0f;
        f32 HeightScale = 64.0f;

        // Tile heightmap resolution (quads per side + 1 for border)
        u32 TileResolution = 513;

        // How many tiles to keep loaded around the camera (radius in tiles)
        u32 LoadRadius = 3;

        // Total tile budget (LRU eviction when exceeded)
        u32 MaxLoadedTiles = 25;

        // Tessellation settings passed to each tile
        bool TessellationEnabled = true;
        f32 TargetTriangleSize = 8.0f;
        f32 MorphRegion = 0.3f;

        // Base directory for tile files (e.g., "assets/terrain/tiles/")
        std::string TileDirectory;

        // Pattern for tile filenames, %d/%d replaced by GridX/GridZ
        // e.g., "tile_%d_%d.raw"
        std::string TileFilePattern = "tile_%d_%d.raw";
    };

    // Manages a grid of terrain tiles, streaming them in/out based on camera proximity.
    // Uses an LRU cache with configurable tile budget and async loading via the Task system.
    class TerrainStreamer : public RefCounted
    {
      public:
        TerrainStreamer() = default;
        ~TerrainStreamer() override;

        void Initialize(const TerrainStreamerConfig& config);

        // Call each frame with the camera position. Determines which tiles are needed,
        // queues loads for missing tiles, and evicts tiles over the budget.
        void Update(const glm::vec3& cameraPos, u64 frameNumber);

        // Process completed async loads on the main thread (GPU upload)
        void ProcessCompletedLoads();

        // Get all ready tiles for rendering
        void GetReadyTiles(std::vector<Ref<TerrainTile>>& outTiles) const;

        // Set shared material for all tiles
        void SetMaterial(const Ref<TerrainMaterial>& material);

        // Stitch edges between loaded neighboring tiles
        void StitchLoadedTiles();

        [[nodiscard]] const TerrainStreamerConfig& GetConfig() const { return m_Config; }
        [[nodiscard]] u32 GetLoadedTileCount() const;
        [[nodiscard]] u32 GetLoadingTileCount() const;

        // Get the tile at a given grid coordinate (may be null if not loaded)
        [[nodiscard]] Ref<TerrainTile> GetTile(i32 gridX, i32 gridZ) const;

        // Force-unload all tiles
        void UnloadAll();

      private:
        // Build the file path for a tile at the given grid coordinates
        [[nodiscard]] std::string BuildTilePath(i32 gridX, i32 gridZ) const;

        // Request async load for a tile
        void RequestTileLoad(i32 gridX, i32 gridZ);

        // Evict least-recently-used tiles until under budget
        void EvictOverBudget();

        TerrainStreamerConfig m_Config;
        Ref<TerrainMaterial> m_SharedMaterial;

        // All known tiles, keyed by grid coordinate
        std::unordered_map<TileCoord, Ref<TerrainTile>, TileCoordHash> m_Tiles;

        // Track in-flight async load tasks
        struct PendingLoad
        {
            TileCoord Coord;
            Tasks::TTask<bool> Task;
            Ref<TerrainTile> Tile;
        };
        std::vector<PendingLoad> m_PendingLoads;

        // Protects m_Tiles during async load completion
        mutable std::mutex m_TileMutex;
    };
} // namespace OloEngine
