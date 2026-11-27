#include "OloEnginePCH.h"
#include "OloEngine/Scene/TerrainGround.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainFrame.h"
#include "OloEngine/Terrain/TerrainStreamer.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine
{
    TerrainGroundSampler::TerrainGroundSampler(const Scene& scene)
    {
        const auto add = [this](const Ref<TerrainData>& data, const TerrainFrame& frame, f32 sizeX, f32 sizeZ, f32 heightScale)
        {
            if (!data || data->GetResolution() < 2u || !(sizeX > 0.0f) || !(sizeZ > 0.0f) || !frame.SupportsVerticalHeight())
                return;
            m_Terrains.Add(Terrain{ data.Raw(), frame.Model, glm::inverse(frame.Model), sizeX, sizeZ, heightScale });
        };
        const auto terrains = scene.GetAllEntitiesWith<TerrainComponent>();
        for (const auto entity : terrains)
        {
            const auto& terrain = terrains.get<TerrainComponent>(entity);
            const TerrainFrame frame{ scene.ComputeWorldTransform(entity) };
            if (!terrain.m_StreamingEnabled)
            {
                add(terrain.m_TerrainData, frame, terrain.m_WorldSizeX, terrain.m_WorldSizeZ, terrain.m_HeightScale);
            }
            else if (terrain.m_Streamer)
            {
                TArray<Ref<TerrainTile>> ready;
                terrain.m_Streamer->GetReadyTiles(ready);
                for (const auto& tile : ready)
                {
                    m_ResidentTiles.Add(tile->GetTerrainData());
                    add(tile->GetTerrainData(), TerrainFrame{ frame.TileModel(tile->WorldOrigin) },
                        tile->WorldSizeX, tile->WorldSizeZ, tile->HeightScale);
                }
            }
        }
    }

    std::optional<f32> TerrainGroundSampler::HeightAt(f32 x, f32 z) const
    {
        if (!std::isfinite(x) || !std::isfinite(z))
            return std::nullopt;
        for (const Terrain& terrain : m_Terrains)
        {
            const glm::vec4 local = terrain.InverseWorld * glm::vec4(x, 0.0f, z, 1.0f);
            const f32 nx = local.x / terrain.SizeX;
            const f32 nz = local.z / terrain.SizeZ;
            if (!std::isfinite(nx) || !std::isfinite(nz) || nx < 0.0f || nx > 1.0f || nz < 0.0f || nz > 1.0f)
                continue;
            const f32 height = terrain.Data->GetHeightAt(nx, nz) * terrain.HeightScale;
            const f32 worldY = (terrain.World * glm::vec4(local.x, height, local.z, 1.0f)).y;
            if (std::isfinite(worldY))
                return worldY;
        }
        return std::nullopt;
    }

    std::optional<f32> SampleTerrainGroundHeight(const Scene& scene, f32 x, f32 z)
    {
        return TerrainGroundSampler(scene).HeightAt(x, z);
    }
} // namespace OloEngine
