#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainTile.h"

namespace OloEngine
{
    bool TerrainTile::LoadFromFile(const std::string& heightmapPath)
    {
        OLO_PROFILE_FUNCTION();

        auto terrainData = Ref<TerrainData>::Create();
        if (!terrainData->LoadFromFile(heightmapPath))
        {
            OLO_CORE_ERROR("TerrainTile[{},{}]: Failed to load heightmap '{}'", GridX, GridZ, heightmapPath);
            return false;
        }

        m_TerrainData = terrainData;
        TileResolution = terrainData->GetResolution();
        return true;
    }

    void TerrainTile::CreateFlat(u32 resolution, f32 defaultHeight)
    {
        OLO_PROFILE_FUNCTION();

        auto terrainData = Ref<TerrainData>::Create();
        terrainData->CreateFlat(resolution, defaultHeight);
        m_TerrainData = terrainData;
        TileResolution = resolution;
    }

    void TerrainTile::BuildGPUResources(bool tessellationEnabled, f32 targetTriangleSize, f32 morphRegion)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_TerrainData)
        {
            OLO_CORE_ERROR("TerrainTile[{},{}]: No terrain data to build GPU resources from", GridX, GridZ);
            return;
        }

        m_ChunkManager = Ref<TerrainChunkManager>::Create();
        m_ChunkManager->TessellationEnabled = tessellationEnabled;

        auto& quadtree = m_ChunkManager->GetQuadtree();
        auto& lodConfig = quadtree.GetConfig();
        lodConfig.TargetTriangleSize = targetTriangleSize;
        lodConfig.MorphRegion = morphRegion;

        m_ChunkManager->GenerateAllChunks(*m_TerrainData, WorldSizeX, WorldSizeZ, HeightScale);

        SetState(State::Ready);
        OLO_CORE_TRACE("TerrainTile[{},{}]: GPU resources built ({} chunks)", GridX, GridZ, m_ChunkManager->GetTotalChunks());
    }

    void TerrainTile::Unload()
    {
        OLO_PROFILE_FUNCTION();

        m_ChunkManager = nullptr;
        m_TerrainData = nullptr;
        m_Material = nullptr;
        SetState(State::Unloaded);
    }

    void TerrainTile::StitchEdge(const TerrainTile& neighbor, u32 direction)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_TerrainData || !neighbor.m_TerrainData)
        {
            return;
        }

        auto& myHeights = m_TerrainData->GetHeightData();
        const auto& neighborHeights = neighbor.m_TerrainData->GetHeightData();
        u32 myRes = m_TerrainData->GetResolution();
        u32 neighborRes = neighbor.m_TerrainData->GetResolution();

        if (myRes == 0 || neighborRes == 0)
        {
            return;
        }

        u32 edgeSamples = std::min(myRes, neighborRes);

        switch (direction)
        {
            case 0: // +X edge: my last column = neighbor's first column
            {
                for (u32 z = 0; z < edgeSamples; ++z)
                {
                    f32 t = static_cast<f32>(z) / static_cast<f32>(edgeSamples - 1);
                    u32 myZ = static_cast<u32>(t * static_cast<f32>(myRes - 1));
                    u32 nZ = static_cast<u32>(t * static_cast<f32>(neighborRes - 1));
                    f32 avg = (myHeights[static_cast<sizet>(myZ) * myRes + (myRes - 1)] +
                               neighborHeights[static_cast<sizet>(nZ) * neighborRes]) * 0.5f;
                    myHeights[static_cast<sizet>(myZ) * myRes + (myRes - 1)] = avg;
                }
                break;
            }
            case 1: // -X edge: my first column = neighbor's last column
            {
                for (u32 z = 0; z < edgeSamples; ++z)
                {
                    f32 t = static_cast<f32>(z) / static_cast<f32>(edgeSamples - 1);
                    u32 myZ = static_cast<u32>(t * static_cast<f32>(myRes - 1));
                    u32 nZ = static_cast<u32>(t * static_cast<f32>(neighborRes - 1));
                    f32 avg = (myHeights[static_cast<sizet>(myZ) * myRes] +
                               neighborHeights[static_cast<sizet>(nZ) * neighborRes + (neighborRes - 1)]) * 0.5f;
                    myHeights[static_cast<sizet>(myZ) * myRes] = avg;
                }
                break;
            }
            case 2: // +Z edge: my last row = neighbor's first row
            {
                for (u32 x = 0; x < edgeSamples; ++x)
                {
                    f32 t = static_cast<f32>(x) / static_cast<f32>(edgeSamples - 1);
                    u32 myX = static_cast<u32>(t * static_cast<f32>(myRes - 1));
                    u32 nX = static_cast<u32>(t * static_cast<f32>(neighborRes - 1));
                    f32 avg = (myHeights[static_cast<sizet>(myRes - 1) * myRes + myX] +
                               neighborHeights[nX]) * 0.5f;
                    myHeights[static_cast<sizet>(myRes - 1) * myRes + myX] = avg;
                }
                break;
            }
            case 3: // -Z edge: my first row = neighbor's last row
            {
                for (u32 x = 0; x < edgeSamples; ++x)
                {
                    f32 t = static_cast<f32>(x) / static_cast<f32>(edgeSamples - 1);
                    u32 myX = static_cast<u32>(t * static_cast<f32>(myRes - 1));
                    u32 nX = static_cast<u32>(t * static_cast<f32>(neighborRes - 1));
                    f32 avg = (myHeights[myX] +
                               neighborHeights[static_cast<sizet>(neighborRes - 1) * neighborRes + nX]) * 0.5f;
                    myHeights[myX] = avg;
                }
                break;
            }
            default:
                break;
        }
    }
} // namespace OloEngine
