#include "OloEnginePCH.h"
#include "OloEngine/Scene/TerrainGround.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine
{
    std::optional<f32> SampleTerrainGroundHeight(const Scene& scene, f32 x, f32 z)
    {
        if (!std::isfinite(x) || !std::isfinite(z))
            return std::nullopt;
        const auto terrains = scene.GetAllEntitiesWith<TerrainComponent>();
        for (const auto entity : terrains)
        {
            const auto& terrain = terrains.get<TerrainComponent>(entity);
            if (!terrain.m_TerrainData || terrain.m_TerrainData->GetResolution() < 2u ||
                !(terrain.m_WorldSizeX > 0.0f) || !(terrain.m_WorldSizeZ > 0.0f))
            {
                continue;
            }
            const glm::mat4 world = scene.GetWorldTransform(entity);
            const glm::vec4 local = glm::inverse(world) * glm::vec4(x, 0.0f, z, 1.0f);
            const f32 nx = local.x / terrain.m_WorldSizeX;
            const f32 nz = local.z / terrain.m_WorldSizeZ;
            if (nx < 0.0f || nx > 1.0f || nz < 0.0f || nz > 1.0f)
                continue;
            const f32 height = terrain.m_TerrainData->GetHeightAt(nx, nz) * terrain.m_HeightScale;
            const f32 worldY = (world * glm::vec4(local.x, height, local.z, 1.0f)).y;
            if (std::isfinite(worldY))
                return worldY;
        }
        return std::nullopt;
    }
} // namespace OloEngine
