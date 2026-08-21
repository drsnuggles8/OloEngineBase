#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

// =============================================================================
// Thread-group ID swizzle — CPU contract test (issue #720).
//
// OLO_TEST_LAYER: shaderpipe
//
// Mirrors OloSwizzleWorkGroupID2D from
// OloEditor/assets/shaders/include/ThreadGroupSwizzle.glsl byte-for-byte (same
// integer arithmetic, same order of operations) so the remap's core property —
// that it is a BIJECTION over the dispatch grid for any tile width — is pinned
// without a GL context. This is what makes the swizzle "purely a coordinate
// remap": if OloSwizzleWorkGroupID2D visits every (x, y) exactly once, then
// substituting its output for gl_WorkGroupID before computing a pixel/voxel/
// cluster coordinate cannot change WHAT a compute shader computes, only the
// order workgroups visit their outputs in — the algorithm's correctness does
// not depend on which of the issue's seven candidate shaders end up adopting
// it (five do; GTAO.comp and HZB.comp were measured to regress instead — see
// ThreadGroupSwizzle.glsl). The live-editor / GPU-timing verification (see
// the PR) checks the per-pass adoption decisions; this test checks the
// algorithm itself is correct for any grid shape.
//
// KEEP THIS IN SYNC with ThreadGroupSwizzle.glsl if the algorithm changes.
// =============================================================================

namespace
{
    // Byte-for-byte mirror of OloSwizzleWorkGroupID2D (ThreadGroupSwizzle.glsl).
    std::pair<int, int> SwizzleWorkGroupID2D(int workGroupIdX, int workGroupIdY, int numWorkGroupsX,
                                             int numWorkGroupsY, int tileWidth)
    {
        tileWidth = std::max(tileWidth, 1);

        int flatIndex = workGroupIdY * numWorkGroupsX + workGroupIdX;
        int groupsPerTile = tileWidth * numWorkGroupsY;

        int tileIndex = flatIndex / groupsPerTile;
        int indexInTile = flatIndex - tileIndex * groupsPerTile;

        int tileColumns = std::max(std::min(tileWidth, numWorkGroupsX - tileIndex * tileWidth), 1);

        int localY = indexInTile / tileColumns;
        int localX = indexInTile - localY * tileColumns;

        return { tileIndex * tileWidth + localX, localY };
    }

    // Applies the swizzle to every workgroup in a numWorkGroupsX x numWorkGroupsY
    // grid and asserts the result is a bijection: every output coordinate is
    // produced by exactly one input, and every input coordinate the dispatch
    // actually contains (0..numWorkGroupsX-1, 0..numWorkGroupsY-1) is covered.
    void AssertIsBijection(int numWorkGroupsX, int numWorkGroupsY, int tileWidth)
    {
        std::set<std::pair<int, int>> seenOutputs;
        for (int y = 0; y < numWorkGroupsY; ++y)
        {
            for (int x = 0; x < numWorkGroupsX; ++x)
            {
                auto [outX, outY] = SwizzleWorkGroupID2D(x, y, numWorkGroupsX, numWorkGroupsY, tileWidth);

                ASSERT_GE(outX, 0) << "grid=" << numWorkGroupsX << "x" << numWorkGroupsY << " tile=" << tileWidth
                                   << " in=(" << x << "," << y << ")";
                ASSERT_LT(outX, numWorkGroupsX) << "grid=" << numWorkGroupsX << "x" << numWorkGroupsY
                                                << " tile=" << tileWidth << " in=(" << x << "," << y << ")";
                ASSERT_GE(outY, 0) << "grid=" << numWorkGroupsX << "x" << numWorkGroupsY << " tile=" << tileWidth
                                   << " in=(" << x << "," << y << ")";
                ASSERT_LT(outY, numWorkGroupsY) << "grid=" << numWorkGroupsX << "x" << numWorkGroupsY
                                                << " tile=" << tileWidth << " in=(" << x << "," << y << ")";

                auto [it, inserted] = seenOutputs.insert({ outX, outY });
                ASSERT_TRUE(inserted) << "duplicate output (" << outX << "," << outY << ") for grid="
                                      << numWorkGroupsX << "x" << numWorkGroupsY << " tile=" << tileWidth
                                      << " — not a bijection";
            }
        }

        ASSERT_EQ(seenOutputs.size(), static_cast<sizet>(numWorkGroupsX) * static_cast<sizet>(numWorkGroupsY));
    }
} // namespace

TEST(ThreadGroupSwizzle, IsBijectionForExactMultipleGrids)
{
    // Dispatch width IS an exact multiple of the tile width — the simple case.
    AssertIsBijection(16, 9, 8);
    AssertIsBijection(64, 64, 8);
    AssertIsBijection(8, 8, 8);
    AssertIsBijection(24, 24, 4);
}

TEST(ThreadGroupSwizzle, IsBijectionForNonMultipleGrids)
{
    // Dispatch width is NOT a multiple of the tile width — exercises the
    // narrower-last-tile-column path (tileColumns < tileWidth).
    AssertIsBijection(5, 3, 2);
    AssertIsBijection(5, 3, 3);
    AssertIsBijection(17, 11, 8);
    AssertIsBijection(121, 68, 16);
    AssertIsBijection(1, 1, 8);
    AssertIsBijection(3, 200, 8); // taller than wide, narrow dispatch
    AssertIsBijection(200, 3, 8); // wider than tall
}

TEST(ThreadGroupSwizzle, IsBijectionForRealPassDispatchShapes)
{
    // The actual dispatch dimensions these passes compute at a representative
    // resolution and tile widths (only X/Y matter here). GTAO/HZB's own
    // shapes are included even though those two shaders don't adopt the
    // swizzle (see ThreadGroupSwizzle.glsl) — the algorithm must still be
    // correct for their grid shapes, since any future re-adoption (a
    // different GPU, a different tile width) depends on it.
    for (int tileWidth : { 1, 4, 8, 16 })
    {
        AssertIsBijection((1920 + 15) / 16, (1080 + 15) / 16, tileWidth); // GTAO/AutoExposureHistogram @ 1080p, local 16x16
        AssertIsBijection((1920 + 7) / 8, (1080 + 7) / 8, tileWidth);     // GTAO_Denoise/HZB/FroxelFogIntegrate @ 1080p, local 8x8
        AssertIsBijection(static_cast<int>(OloEngine::ClusteredLighting::kClusterCountX),
                          static_cast<int>(OloEngine::ClusteredLighting::kClusterCountY), tileWidth); // the real LightCulling cluster grid XY
    }
}

TEST(ThreadGroupSwizzle, TileWidthOfOneIsStillABijection)
{
    // tileWidth = 1 degenerates to column-major order (the maximally "narrow"
    // tile) rather than the identity — still must visit every coordinate once.
    AssertIsBijection(10, 7, 1);
}

TEST(ThreadGroupSwizzle, TileWiderThanDispatchDegeneratesSafely)
{
    // tileWidth > numWorkGroups.x: the whole grid is one tile. Must not divide
    // by a zero-width tileColumns and must still be a bijection.
    AssertIsBijection(3, 5, 8);
    AssertIsBijection(1, 1, 64);
}

TEST(ThreadGroupSwizzle, KnownValuesForHandVerifiedExample)
{
    // Hand-verified in the PR description: grid 5x3, tileWidth 2. Tile 0 covers
    // columns {0,1} (full height 3), tile 1 covers {2,3}, tile 2 (partial,
    // width 1) covers {4}.
    EXPECT_EQ(SwizzleWorkGroupID2D(0, 0, 5, 3, 2), std::make_pair(0, 0));
    EXPECT_EQ(SwizzleWorkGroupID2D(1, 0, 5, 3, 2), std::make_pair(1, 0));
    EXPECT_EQ(SwizzleWorkGroupID2D(2, 0, 5, 3, 2), std::make_pair(0, 1));
    EXPECT_EQ(SwizzleWorkGroupID2D(3, 0, 5, 3, 2), std::make_pair(1, 1));
    EXPECT_EQ(SwizzleWorkGroupID2D(4, 0, 5, 3, 2), std::make_pair(0, 2));
    // Row 1: flatIndex 5..9 (still tile 0, indices 5..7, then tile 1, 8..9)
    EXPECT_EQ(SwizzleWorkGroupID2D(0, 1, 5, 3, 2), std::make_pair(1, 2));
    EXPECT_EQ(SwizzleWorkGroupID2D(1, 1, 5, 3, 2), std::make_pair(2, 0));
    EXPECT_EQ(SwizzleWorkGroupID2D(2, 1, 5, 3, 2), std::make_pair(3, 0));
    // Row 2, x=4: flatIndex 14 -> tileIndex 2 (groupsPerTile=6), indexInTile=2
    EXPECT_EQ(SwizzleWorkGroupID2D(4, 2, 5, 3, 2), std::make_pair(4, 2));
}
