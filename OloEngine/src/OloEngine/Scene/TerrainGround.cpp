#include "OloEnginePCH.h"
#include "OloEngine/Scene/TerrainGround.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine
{
    TerrainGroundSampler::TerrainGroundSampler(const Scene& scene)
    {
        const auto terrains = scene.GetAllEntitiesWith<TerrainComponent>();
        for (const auto entity : terrains)
        {
            const auto& terrain = terrains.get<TerrainComponent>(entity);
            if (!terrain.m_TerrainData || terrain.m_TerrainData->GetResolution() < 2u ||
                !(terrain.m_WorldSizeX > 0.0f) || !(terrain.m_WorldSizeZ > 0.0f))
            {
                continue;
            }
            Terrain entry;
            entry.Data = terrain.m_TerrainData.Raw();
            entry.World = scene.GetWorldTransform(entity);
            entry.InverseWorld = glm::inverse(entry.World);
            entry.SizeX = terrain.m_WorldSizeX;
            entry.SizeZ = terrain.m_WorldSizeZ;
            entry.HeightScale = terrain.m_HeightScale;
            m_Terrains.Add(entry);
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
            if (nx < 0.0f || nx > 1.0f || nz < 0.0f || nz > 1.0f)
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
