#include "OloEnginePCH.h"
#include "FoliagePlacement.h"

#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <cmath>
#include <cstring>
#include <glm/gtc/constants.hpp>

namespace OloEngine::FoliagePlacement
{
    f32 HashPosition(f32 x, f32 z, u32 seed)
    {
        u32 hx = 0;
        u32 hz = 0;
        f32 fx = x * 73856093.0f;
        f32 fz = z * 19349663.0f;
        std::memcpy(&hx, &fx, sizeof(u32));
        std::memcpy(&hz, &fz, sizeof(u32));
        u32 h = hx ^ hz ^ seed;
        h = (h * 2654435761u) >> 16;
        return static_cast<f32>(h & 0xFFFF) / 65536.0f;
    }

    f32 SpacingForDensity(f32 density)
    {
        return 1.0f / std::sqrt(density);
    }

    void GenerateLayer(const FoliageLayer& layer, u32 layerIndex,
                       const std::vector<f32>& heights, u32 resolution,
                       const TerrainMaterial* material,
                       f32 worldSizeX, f32 worldSizeZ, f32 heightScale,
                       std::vector<Placement>& out)
    {
        OLO_PROFILE_FUNCTION();

        out.clear();

        if (!layer.Enabled || layer.Density <= 0.0f || resolution == 0)
        {
            return;
        }

        const f32 spacing = SpacingForDensity(layer.Density);
        const auto countX = static_cast<u32>(std::ceil(worldSizeX / spacing));
        const auto countZ = static_cast<u32>(std::ceil(worldSizeZ / spacing));

        // Splatmap data for density masking
        const u8* splatData = nullptr;
        u32 splatRes = 0;
        if (material && layer.SplatmapChannel >= 0 && layer.SplatmapChannel < 8)
        {
            const i32 splatIdx = layer.SplatmapChannel / 4;
            if (material->HasCPUSplatmaps())
            {
                const auto& splatmapVec = material->GetSplatmapData(static_cast<u32>(splatIdx));
                if (!splatmapVec.empty())
                {
                    splatData = splatmapVec.data();
                    splatRes = material->GetSplatmapResolution();
                }
            }
        }

        const f32 cosMinSlope = std::cos(glm::radians(layer.MaxSlopeAngle));
        const f32 cosMaxSlope = std::cos(glm::radians(layer.MinSlopeAngle));

        out.reserve(static_cast<sizet>(countX) * countZ / 4); // Estimate ~25% coverage

        const u32 seed = SeedForLayer(layerIndex);

        for (u32 iz = 0; iz < countZ; ++iz)
        {
            for (u32 ix = 0; ix < countX; ++ix)
            {
                // Jittered position
                const f32 jx = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed);
                const f32 jz = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 7);

                const f32 worldX = (static_cast<f32>(ix) + jx) * spacing;
                const f32 worldZ = (static_cast<f32>(iz) + jz) * spacing;

                if (worldX >= worldSizeX || worldZ >= worldSizeZ)
                    continue;

                const f32 nx = worldX / worldSizeX;
                const f32 nz = worldZ / worldSizeZ;

                // Slope check — dot(normal, up)
                const glm::vec3 normal = TerrainData::SampleNormal(heights, resolution, nx, nz,
                                                                   worldSizeX, worldSizeZ, heightScale);
                if (const f32 upDot = normal.y; upDot < cosMinSlope || upDot > cosMaxSlope)
                    continue;

                // Splatmap density check
                if (splatData && splatRes > 0)
                {
                    const u32 sx = std::min(static_cast<u32>(nx * static_cast<f32>(splatRes)), splatRes - 1);
                    const u32 sz = std::min(static_cast<u32>(nz * static_cast<f32>(splatRes)), splatRes - 1);
                    const i32 channelInSplat = layer.SplatmapChannel % 4;
                    // Splatmap is RGBA packed, so index = (sz * splatRes + sx) * 4 + channel
                    const f32 splatWeight = static_cast<f32>(splatData[(sz * splatRes + sx) * 4 + channelInSplat]) / 255.0f;
                    const f32 threshold = HashPosition(static_cast<f32>(ix) + 0.5f, static_cast<f32>(iz) + 0.5f, seed + 13);
                    if (threshold > splatWeight)
                        continue;
                }

                const f32 height = TerrainData::SampleHeight(heights, resolution, nx, nz) * heightScale;

                // Randomize scale and height
                const f32 scaleRand = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 3);
                const f32 heightRand = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 5);
                const f32 scale = glm::mix(layer.MinScale, layer.MaxScale, scaleRand);
                const f32 instanceHeight = glm::mix(layer.MinHeight, layer.MaxHeight, heightRand);

                // Random rotation
                f32 rotation = 0.0f;
                if (layer.RandomRotation)
                {
                    rotation = HashPosition(static_cast<f32>(ix), static_cast<f32>(iz), seed + 11) * glm::two_pi<f32>();
                }

                Placement placement;
                placement.m_CellX = ix;
                placement.m_CellZ = iz;
                placement.m_Row.PositionScale = glm::vec4(worldX, height, worldZ, scale);
                placement.m_Row.RotationHeight = glm::vec4(rotation, instanceHeight, 1.0f, 0.0f); // fade=1 (full)
                placement.m_Row.ColorAlpha = glm::vec4(layer.BaseColor, layer.AlphaCutoff);
                out.push_back(placement);
            }
        }
    }
} // namespace OloEngine::FoliagePlacement
