#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <vector>

namespace OloEngine
{
    class Tileset;
    struct TilemapComponent;

    // @brief An axis-aligned run of solid tiles, in tile coordinates.
    //
    // Covers the half-open cell range [X, X + Width) x [Y, Y + Height). One
    // Box2D polygon shape is created per rect.
    struct TileColliderRect
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Width = 0;
        u32 Height = 0;

        auto operator==(const TileColliderRect&) const -> bool = default;
    };

    // @brief Merges solid tiles into as few rectangles as possible.
    //
    // Greedy rectangle meshing: scan row-major, and at each unclaimed solid cell
    // grow right as far as the run continues, then grow up as far as every cell of
    // that width stays solid and unclaimed. One body per tilemap with a handful of
    // wide shapes is dramatically cheaper for Box2D than one box per tile, and it
    // also removes the internal edges that make a character catch on tile seams.
    //
    // The output is deterministic (row-major emission order), which is what makes
    // it testable without a physics world.
    namespace TilemapCollider
    {
        // @param solid  row-major `width * height` mask, indexed `y * width + x`.
        //               A mask shorter than that is treated as non-solid past its end.
        [[nodiscard]] std::vector<TileColliderRect> MergeSolidRuns(const std::vector<bool>& solid, u32 width, u32 height);

        // @brief The solid mask of a tilemap: the union of every `Solid` layer's
        //        tiles whose tileset entry is marked solid.
        //
        // A null tileset yields an all-false mask — without per-tile metadata there
        // is nothing to call solid, and inventing collision would be worse than none.
        [[nodiscard]] std::vector<bool> BuildSolidMask(const TilemapComponent& tilemap, const Ref<Tileset>& tileset);

        // Convenience: BuildSolidMask followed by MergeSolidRuns.
        [[nodiscard]] std::vector<TileColliderRect> BuildColliderRects(const TilemapComponent& tilemap, const Ref<Tileset>& tileset);
    } // namespace TilemapCollider
} // namespace OloEngine
