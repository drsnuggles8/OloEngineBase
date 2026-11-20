// OLO_TEST_LAYER: L1
//
// The rule that picks which of its meshes a terrain chunk casts its shadow from
// (issue #1721): the coarsest one whose quads are no larger than the triangles
// the main view draws that ground with.
//
// The cost this exists for: a 1024 px terrain is two million shadow patches at
// full density, drawn again in every cascade it falls in, however far away it
// is. The rule is a pure function of distance and the main view's projection,
// so it is pinned here without a GPU. What these cannot see is the picture: that
// a coarser caster leaves the shadow where it was is a device fact, checked by
// the Drift island evidence captures and in the live editor.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Terrain/TerrainChunk.h"
#include "OloEngine/Terrain/TerrainShadowLod.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        // Drift's Ridgeback: a 420 m tile on a 1024 px height map is 16 chunks
        // of 64 quads a side, so a quad is 420 / (16 * 64) m.
        constexpr f32 kQuad = 420.0f / (16.0f * 64.0f);
        constexpr f32 kTargetPixels = 6.0f;
        constexpr f32 kViewportHeight = 1080.0f;
        constexpr f32 kFovY = 1.0471975512f; // 60 degrees

        [[nodiscard]] f32 PixelPerMetre()
        {
            const glm::mat4 projection = glm::perspective(kFovY, 16.0f / 9.0f, 0.2f, 4000.0f);
            const glm::mat4 view = glm::lookAt(glm::vec3(40.0f, 12.0f, -90.0f), glm::vec3(0.0f, 30.0f, 280.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            return TerrainShadowLod::PixelSizePerMetre(projection * view, kViewportHeight);
        }
    } // namespace

    // Derivation: at distance d the view is 2 d tan(fovY / 2) high and has
    // viewportHeight pixels in that, so a pixel is 2 tan(fovY / 2) / height
    // metres per metre of distance. The camera's pose does not enter.
    TEST(TerrainShadowLod, APixelsWorldSizeFollowsTheFieldOfViewAndTheViewportHeight)
    {
        const f32 expected = 2.0f * std::tan(kFovY * 0.5f) / kViewportHeight;
        EXPECT_NEAR(PixelPerMetre(), expected, expected * 1.0e-4f);

        // Vulkan's projection flips Y; the size of a pixel is the same.
        glm::mat4 flipped = glm::perspective(kFovY, 16.0f / 9.0f, 0.2f, 4000.0f);
        flipped[1][1] *= -1.0f;
        EXPECT_NEAR(TerrainShadowLod::PixelSizePerMetre(flipped, kViewportHeight), expected, expected * 1.0e-4f);
    }

    // An orthographic view has no "per metre of distance", and a view with no
    // height has no pixels: both answer zero, which Select reads as level 0.
    TEST(TerrainShadowLod, AViewWithNoPerspectiveKeepsEveryChunkAtFullDensity)
    {
        const glm::mat4 ortho = glm::ortho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 500.0f);
        EXPECT_EQ(TerrainShadowLod::PixelSizePerMetre(ortho, kViewportHeight), 0.0f);
        EXPECT_EQ(TerrainShadowLod::PixelSizePerMetre(glm::mat4(0.0f), kViewportHeight), 0.0f);
        EXPECT_EQ(TerrainShadowLod::PixelSizePerMetre(glm::perspective(kFovY, 1.0f, 0.1f, 10.0f), 0.0f), 0.0f);
        EXPECT_EQ(TerrainShadowLod::Select(900.0f, kQuad, 0.0f, kTargetPixels), 0u);
    }

    // The promise: the mesh a chunk casts from is never coarser than the
    // triangles the main view draws it with, and never finer than it has to be.
    TEST(TerrainShadowLod, TheCasterIsAsFineAsTheReceiverAndNoFiner)
    {
        const f32 pixelPerMetre = PixelPerMetre();
        ASSERT_GT(pixelPerMetre, 0.0f);
        u32 previous = 0;
        for (f32 distance = 0.0f; distance < 2000.0f; distance += 3.0f)
        {
            const u32 level = TerrainShadowLod::Select(distance, kQuad, pixelPerMetre, kTargetPixels);
            ASSERT_LT(level, TerrainShadowLod::kLevelCount);
            EXPECT_GE(level, previous) << "the level dropped as the chunk moved away, at " << distance << " m";
            previous = level;

            const f32 receiver = distance * pixelPerMetre * kTargetPixels;
            const f32 casterQuad = kQuad * static_cast<f32>(1u << level);
            if (level > 0)
                EXPECT_LE(casterQuad, receiver) << "caster coarser than the receiver at " << distance << " m";
            if (level + 1 < TerrainShadowLod::kLevelCount)
                EXPECT_GT(casterQuad * 2.0f, receiver) << "a coarser level would still have done at " << distance << " m";
        }
        // Close up it is the chunk mesh itself; far away, the coarsest.
        EXPECT_EQ(TerrainShadowLod::Select(0.0f, kQuad, pixelPerMetre, kTargetPixels), 0u);
        EXPECT_EQ(TerrainShadowLod::Select(20.0f, kQuad, pixelPerMetre, kTargetPixels), 0u);
        EXPECT_EQ(previous, TerrainShadowLod::kLevelCount - 1u);
    }

    TEST(TerrainShadowLod, BadInputsMeanFullDensity)
    {
        const f32 pixelPerMetre = PixelPerMetre();
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        const f32 inf = std::numeric_limits<f32>::infinity();
        EXPECT_EQ(TerrainShadowLod::Select(nan, kQuad, pixelPerMetre, kTargetPixels), 0u);
        EXPECT_EQ(TerrainShadowLod::Select(inf, kQuad, pixelPerMetre, kTargetPixels), 0u);
        EXPECT_EQ(TerrainShadowLod::Select(500.0f, 0.0f, pixelPerMetre, kTargetPixels), 0u);
        EXPECT_EQ(TerrainShadowLod::Select(500.0f, -1.0f, pixelPerMetre, kTargetPixels), 0u);
        EXPECT_EQ(TerrainShadowLod::Select(500.0f, kQuad, nan, kTargetPixels), 0u);
        EXPECT_EQ(TerrainShadowLod::Select(500.0f, kQuad, pixelPerMetre, 0.0f), 0u);
        EXPECT_EQ(TerrainShadowLod::Select(-5.0f, kQuad, pixelPerMetre, kTargetPixels), 0u);
    }

    // The coarsest level still has to be a grid: the stride of the last level
    // must divide a chunk's side.
    TEST(TerrainShadowLod, EveryLevelDividesAChunk)
    {
        for (u32 level = 0; level < TerrainShadowLod::kLevelCount; ++level)
        {
            const u32 stride = 1u << level;
            EXPECT_EQ(TerrainChunk::CHUNK_RESOLUTION % stride, 0u) << "level " << level;
            EXPECT_GE(TerrainChunk::CHUNK_RESOLUTION / stride, 4u) << "level " << level;
        }
    }
} // namespace OloEngine::Tests
