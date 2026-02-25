#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainStreamer.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace OloEngine
{
    TerrainStreamer::~TerrainStreamer()
    {
        UnloadAll();
    }

    void TerrainStreamer::Initialize(const TerrainStreamerConfig& config)
    {
        OLO_PROFILE_FUNCTION();

        m_Config = config;
        OLO_CORE_INFO("TerrainStreamer: Initialized (tileSize={}, loadRadius={}, budget={})",
                      m_Config.TileWorldSize, m_Config.LoadRadius, m_Config.MaxLoadedTiles);
    }

    void TerrainStreamer::Update(const glm::vec3& cameraPos, u64 frameNumber)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Config.TileWorldSize <= 0.0f)
        {
            return;
        }

        // Determine which tile the camera is in
        i32 cameraTileX = static_cast<i32>(std::floor(cameraPos.x / m_Config.TileWorldSize));
        i32 cameraTileZ = static_cast<i32>(std::floor(cameraPos.z / m_Config.TileWorldSize));

        i32 radius = static_cast<i32>(m_Config.LoadRadius);

        // Mark all tiles in the load radius as needed
        for (i32 dz = -radius; dz <= radius; ++dz)
        {
            for (i32 dx = -radius; dx <= radius; ++dx)
            {
                i32 gx = cameraTileX + dx;
                i32 gz = cameraTileZ + dz;
                TileCoord coord{ gx, gz };

                std::lock_guard lock(m_TileMutex);
                auto it = m_Tiles.find(coord);
                if (it != m_Tiles.end())
                {
                    // Tile exists — update LRU timestamp
                    it->second->LastUsedFrame = frameNumber;
                }
                else
                {
                    // Need to load this tile
                    RequestTileLoad(gx, gz);
                }
            }
        }

        // Process completed async loads (GPU upload on main thread)
        ProcessCompletedLoads();

        // Evict tiles over budget
        EvictOverBudget();
    }

    void TerrainStreamer::ProcessCompletedLoads()
    {
        OLO_PROFILE_FUNCTION();

        auto it = m_PendingLoads.begin();
        while (it != m_PendingLoads.end())
        {
            if (it->Task.IsCompleted())
            {
                auto& tile = it->Tile;
                if (tile->GetState() == TerrainTile::State::Loaded)
                {
                    // Build GPU resources on main thread
                    tile->BuildGPUResources(
                        m_Config.TessellationEnabled,
                        m_Config.TargetTriangleSize,
                        m_Config.MorphRegion);

                    if (m_SharedMaterial)
                    {
                        tile->SetMaterial(m_SharedMaterial);
                    }

                    std::lock_guard lock(m_TileMutex);
                    m_Tiles[it->Coord] = tile;

                    OLO_CORE_TRACE("TerrainStreamer: Tile[{},{}] ready", it->Coord.X, it->Coord.Z);
                }
                else
                {
                    OLO_CORE_WARN("TerrainStreamer: Tile[{},{}] failed to load", it->Coord.X, it->Coord.Z);
                }

                it = m_PendingLoads.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void TerrainStreamer::GetReadyTiles(std::vector<Ref<TerrainTile>>& outTiles) const
    {
        std::lock_guard lock(m_TileMutex);
        outTiles.clear();
        outTiles.reserve(m_Tiles.size());
        for (auto& [coord, tile] : m_Tiles)
        {
            if (tile->GetState() == TerrainTile::State::Ready)
            {
                outTiles.push_back(tile);
            }
        }
    }

    void TerrainStreamer::SetMaterial(const Ref<TerrainMaterial>& material)
    {
        std::lock_guard lock(m_TileMutex);
        m_SharedMaterial = material;
        for (auto& [coord, tile] : m_Tiles)
        {
            tile->SetMaterial(material);
        }
    }

    void TerrainStreamer::StitchLoadedTiles()
    {
        OLO_PROFILE_FUNCTION();

        std::lock_guard lock(m_TileMutex);
        for (auto& [coord, tile] : m_Tiles)
        {
            if (tile->GetState() != TerrainTile::State::Ready)
            {
                continue;
            }

            // Stitch +X neighbor
            auto itPX = m_Tiles.find({ coord.X + 1, coord.Z });
            if (itPX != m_Tiles.end() && itPX->second->GetState() == TerrainTile::State::Ready)
            {
                tile->StitchEdge(*itPX->second, 0);
            }

            // Stitch +Z neighbor
            auto itPZ = m_Tiles.find({ coord.X, coord.Z + 1 });
            if (itPZ != m_Tiles.end() && itPZ->second->GetState() == TerrainTile::State::Ready)
            {
                tile->StitchEdge(*itPZ->second, 2);
            }
        }
    }

    u32 TerrainStreamer::GetLoadedTileCount() const
    {
        std::lock_guard lock(m_TileMutex);
        u32 count = 0;
        for (auto& [coord, tile] : m_Tiles)
        {
            if (tile->GetState() == TerrainTile::State::Ready)
            {
                ++count;
            }
        }
        return count;
    }

    u32 TerrainStreamer::GetLoadingTileCount() const
    {
        return static_cast<u32>(m_PendingLoads.size());
    }

    Ref<TerrainTile> TerrainStreamer::GetTile(i32 gridX, i32 gridZ) const
    {
        std::lock_guard lock(m_TileMutex);
        auto it = m_Tiles.find({ gridX, gridZ });
        if (it != m_Tiles.end())
        {
            return it->second;
        }
        return nullptr;
    }

    void TerrainStreamer::UnloadAll()
    {
        OLO_PROFILE_FUNCTION();

        // Wait for all pending loads to finish
        for (auto& pending : m_PendingLoads)
        {
            pending.Task.Wait();
        }
        m_PendingLoads.clear();

        std::lock_guard lock(m_TileMutex);
        for (auto& [coord, tile] : m_Tiles)
        {
            tile->Unload();
        }
        m_Tiles.clear();
    }

    std::string TerrainStreamer::BuildTilePath(i32 gridX, i32 gridZ) const
    {
        char filename[256];
        std::snprintf(filename, sizeof(filename), m_Config.TileFilePattern.c_str(), gridX, gridZ);
        return m_Config.TileDirectory + "/" + filename;
    }

    void TerrainStreamer::RequestTileLoad(i32 gridX, i32 gridZ)
    {
        OLO_PROFILE_FUNCTION();

        // Check if already pending
        TileCoord coord{ gridX, gridZ };
        for (const auto& pending : m_PendingLoads)
        {
            if (pending.Coord == coord)
            {
                return;
            }
        }

        auto tile = Ref<TerrainTile>::Create();
        tile->GridX = gridX;
        tile->GridZ = gridZ;
        tile->TileResolution = m_Config.TileResolution;
        tile->WorldSizeX = m_Config.TileWorldSize;
        tile->WorldSizeZ = m_Config.TileWorldSize;
        tile->HeightScale = m_Config.HeightScale;
        tile->WorldOrigin = glm::vec3(
            static_cast<f32>(gridX) * m_Config.TileWorldSize,
            0.0f,
            static_cast<f32>(gridZ) * m_Config.TileWorldSize);
        tile->SetState(TerrainTile::State::Loading);

        std::string tilePath = BuildTilePath(gridX, gridZ);

        // Async load: CPU heightmap parsing happens on background thread
        // GPU upload deferred to ProcessCompletedLoads on the main thread
        auto loadTask = Tasks::Launch("TerrainTileLoad", [tile, tilePath]() mutable -> bool
                                      {
                if (std::filesystem::exists(tilePath))
                {
                    if (tile->LoadFromFile(tilePath))
                    {
                        tile->SetState(TerrainTile::State::Loaded);
                        return true;
                    }
                }
                else
                {
                    // No file on disk — create a flat tile
                    tile->CreateFlat(tile->TileResolution, 0.0f);
                    tile->SetState(TerrainTile::State::Loaded);
                    return true;
                }
                tile->SetState(TerrainTile::State::Unloaded);
                return false; }, Tasks::ETaskPriority::BackgroundNormal);

        m_PendingLoads.push_back({ coord, std::move(loadTask), tile });
    }

    void TerrainStreamer::EvictOverBudget()
    {
        OLO_PROFILE_FUNCTION();

        std::lock_guard lock(m_TileMutex);

        if (m_Tiles.size() <= m_Config.MaxLoadedTiles)
        {
            return;
        }

        // Collect tiles sorted by LRU timestamp
        std::vector<std::pair<TileCoord, u64>> sortedTiles;
        sortedTiles.reserve(m_Tiles.size());
        for (auto& [coord, tile] : m_Tiles)
        {
            sortedTiles.push_back({ coord, tile->LastUsedFrame });
        }

        std::sort(sortedTiles.begin(), sortedTiles.end(),
                  [](const auto& a, const auto& b)
                  { return a.second < b.second; });

        // Evict oldest tiles until under budget
        sizet toEvict = m_Tiles.size() - m_Config.MaxLoadedTiles;
        for (sizet i = 0; i < toEvict && i < sortedTiles.size(); ++i)
        {
            auto it = m_Tiles.find(sortedTiles[i].first);
            if (it != m_Tiles.end())
            {
                it->second->Unload();
                m_Tiles.erase(it);
                OLO_CORE_TRACE("TerrainStreamer: Evicted tile[{},{}]",
                               sortedTiles[i].first.X, sortedTiles[i].first.Z);
            }
        }
    }
} // namespace OloEngine
