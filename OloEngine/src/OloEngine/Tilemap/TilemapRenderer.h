#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Frustum.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class Texture2D;
    class Tileset;
    struct TilemapComponent;

    // @brief Draws TilemapComponent grids through the existing Renderer2D batcher.
    //
    // There is no parallel draw path: every tile becomes one Renderer2D quad, so
    // tiles batch together with sprites, share the same texture slots and land in
    // the same draw call. What this adds on top is *chunking and culling* — the
    // grid is divided into kChunkSize x kChunkSize blocks, each block's world AABB
    // is frustum-tested once, and an off-screen block emits nothing.
    //
    // That is what keeps the cost proportional to what is on screen rather than to
    // the map: a 32x32 and a 512x512 map viewed through the same camera submit the
    // same number of quads and the same number of draw calls.
    namespace TilemapRenderer
    {
        // Tiles per chunk edge. 16x16 = 256 tiles is a reasonable culling
        // granularity: small enough that a screen-edge chunk wastes few quads,
        // large enough that the per-chunk AABB test is amortised.
        inline constexpr u32 kChunkSize = 16;

        struct DrawStats
        {
            u32 ChunksTotal = 0;
            u32 ChunksVisible = 0;
            u32 TilesSubmitted = 0;

            auto operator==(const DrawStats&) const -> bool = default;
        };

        // @brief Submit one tilemap's visible tiles.
        //
        // Must be called between Renderer2D::BeginScene and EndScene — it only
        // pushes quads.
        //
        // The tileset and its atlas texture are passed in already resolved rather
        // than looked up here, so this function has no dependency on the asset
        // system: Scene::RenderTilemaps does the AssetManager lookups, and a test
        // can hand it assets it built in memory.
        //
        // @param transform  the entity's world transform; tile (0,0)'s lower-left
        //                   corner sits at its origin.
        // @param frustum    camera frustum for chunk culling, built from the same
        //                   view-projection the batch was begun with.
        // @param entityID   picking id written into every tile vertex.
        // @return per-call counts, for tests and the editor statistics panel.
        //         A null tileset or texture draws nothing and returns zeroes.
        DrawStats Draw(const TilemapComponent& tilemap, const Ref<Tileset>& tileset, const Ref<Texture2D>& texture,
                       const glm::mat4& transform, const Frustum& frustum, int entityID);
    } // namespace TilemapRenderer
} // namespace OloEngine
