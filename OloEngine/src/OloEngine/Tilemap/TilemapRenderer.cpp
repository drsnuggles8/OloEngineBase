#include "OloEnginePCH.h"
#include "OloEngine/Tilemap/TilemapRenderer.h"

#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Tilemap/TilemapComponent.h"
#include "OloEngine/Tilemap/Tileset.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    namespace TilemapRenderer
    {
        namespace
        {
            // World AABB of the local XY box [minLocal, maxLocal] under `transform`.
            // The transform may rotate and shear, so all four corners are mapped and
            // the extremes taken; the Z slab is collapsed to the plane the tiles sit
            // on, then padded so a perfectly edge-on chunk still has a testable box.
            void ChunkWorldBounds(const glm::mat4& transform, const glm::vec2& minLocal, const glm::vec2& maxLocal,
                                  f32 z, glm::vec3& outMin, glm::vec3& outMax)
            {
                const glm::vec4 corners[4] = {
                    transform * glm::vec4(minLocal.x, minLocal.y, z, 1.0f),
                    transform * glm::vec4(maxLocal.x, minLocal.y, z, 1.0f),
                    transform * glm::vec4(maxLocal.x, maxLocal.y, z, 1.0f),
                    transform * glm::vec4(minLocal.x, maxLocal.y, z, 1.0f)
                };

                outMin = glm::vec3(corners[0]);
                outMax = outMin;
                for (int i = 1; i < 4; ++i)
                {
                    outMin = glm::min(outMin, glm::vec3(corners[i]));
                    outMax = glm::max(outMax, glm::vec3(corners[i]));
                }

                // A zero-thickness box is legal for Frustum::IsBoxVisible, but a
                // hair of depth keeps a map lying exactly on a frustum plane from
                // flickering in and out with float rounding.
                constexpr f32 kDepthPad = 1e-3f;
                outMin.z -= kDepthPad;
                outMax.z += kDepthPad;
            }
        } // namespace

        DrawStats Draw(const TilemapComponent& tilemap, const Ref<Tileset>& tileset, const Ref<Texture2D>& texture,
                       const glm::mat4& transform, const Frustum& frustum, int entityID)
        {
            OLO_PROFILE_FUNCTION();

            DrawStats stats;

            if (!tileset || !texture)
                return stats;

            // Slice against the texture we are about to sample rather than the size
            // cached on the Tileset. The cached one is filled in at load time and is
            // merely an optimisation for callers without a texture to hand; a
            // tileset built in memory, or one whose atlas was re-exported at another
            // size, would still have the stale value. Reading the live dimensions
            // also keeps this function from writing to a shared asset mid-frame.
            const u32 atlasWidth = texture->GetWidth();
            const u32 atlasHeight = texture->GetHeight();

            const u32 chunksX = (tilemap.Width + kChunkSize - 1) / kChunkSize;
            const u32 chunksY = (tilemap.Height + kChunkSize - 1) / kChunkSize;
            const f32 tileSize = tilemap.TileSize;

            for (sizet layerIndex = 0; layerIndex < tilemap.Layers.size(); ++layerIndex)
            {
                const auto& layer = tilemap.Layers[layerIndex];
                if (!layer.Visible || layer.Opacity <= 0.0f)
                    continue;

                glm::vec4 tint = tilemap.Color;
                tint.a *= layer.Opacity;

                for (u32 chunkY = 0; chunkY < chunksY; ++chunkY)
                {
                    for (u32 chunkX = 0; chunkX < chunksX; ++chunkX)
                    {
                        ++stats.ChunksTotal;

                        const u32 x0 = chunkX * kChunkSize;
                        const u32 y0 = chunkY * kChunkSize;
                        const u32 x1 = std::min(x0 + kChunkSize, tilemap.Width);
                        const u32 y1 = std::min(y0 + kChunkSize, tilemap.Height);

                        glm::vec3 boundsMin{};
                        glm::vec3 boundsMax{};
                        ChunkWorldBounds(transform,
                                         { static_cast<f32>(x0) * tileSize, static_cast<f32>(y0) * tileSize },
                                         { static_cast<f32>(x1) * tileSize, static_cast<f32>(y1) * tileSize },
                                         layer.ZOffset, boundsMin, boundsMax);
                        if (!frustum.IsBoxVisible(boundsMin, boundsMax))
                            continue;

                        ++stats.ChunksVisible;

                        for (u32 y = y0; y < y1; ++y)
                        {
                            for (u32 x = x0; x < x1; ++x)
                            {
                                const u32 entry = tilemap.GetTile(layerIndex, x, y);
                                if (entry == TilemapComponent::kEmptyTile)
                                    continue;

                                glm::vec2 uvMin{};
                                glm::vec2 uvMax{};
                                if (!tileset->GetTileUVForAtlas(entry - 1, atlasWidth, atlasHeight, uvMin, uvMax))
                                    continue;

                                const glm::vec3 center{ (static_cast<f32>(x) + 0.5f) * tileSize,
                                                        (static_cast<f32>(y) + 0.5f) * tileSize,
                                                        layer.ZOffset };
                                const glm::mat4 tileTransform = transform *
                                                                glm::translate(glm::mat4(1.0f), center) *
                                                                glm::scale(glm::mat4(1.0f), { tileSize, tileSize, 1.0f });

                                Renderer2D::DrawQuad(tileTransform, texture, uvMin, uvMax, tint, entityID);
                                ++stats.TilesSubmitted;
                            }
                        }
                    }
                }
            }

            return stats;
        }
    } // namespace TilemapRenderer
} // namespace OloEngine
