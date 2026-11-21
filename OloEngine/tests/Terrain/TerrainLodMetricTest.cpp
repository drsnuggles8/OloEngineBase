// OLO_TEST_LAYER: L1
//
// The screen-space error a terrain quadtree node is split by (issue #1721):
// the on-screen size of ONE QUAD of the node, from a projection scale that does
// not depend on where the camera looks.
//
// Both halves were wrong at once, and neither showed as a wrong picture. The
// error was the whole node's extent, so against a triangle target of a few
// pixels every node split to the deepest level: all the detail, all the time,
// at a cost that grew with the square of the height map. And the scale was
// viewProjection[1][1], which carries the camera's pitch, so a camera looking
// straight down selected the root node alone.
//
// These pin the metric as arithmetic. That the CPU and GPU descents select the
// same nodes from it is TerrainGPUQuadtreeParityTest's.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Terrain/TerrainLodMetric.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kFovY = 1.0471975512f; // 60 degrees
        constexpr f32 kViewportHeight = 1080.0f;

        [[nodiscard]] glm::mat4 Projection()
        {
            return glm::perspective(kFovY, 16.0f / 9.0f, 0.2f, 4000.0f);
        }

        // cot(fovY / 2) * height / 2 is the pixels a metre spans at one metre;
        // a node's quad is 1 / kNodeQuads of the node.
        [[nodiscard]] f32 ExpectedScale()
        {
            return (1.0f / std::tan(kFovY * 0.5f)) * kViewportHeight * 0.5f / static_cast<f32>(TerrainLodMetric::kNodeQuads);
        }
    } // namespace

    TEST(TerrainLodMetric, TheScaleIsTheProjectionsAndNotTheCamerasPitch)
    {
        const f32 expected = ExpectedScale();
        const glm::vec3 eye(40.0f, 60.0f, -90.0f);
        const struct
        {
            const char* Name;
            glm::vec3 Target;
            glm::vec3 Up;
        } views[] = {
            { "level", { 40.0f, 60.0f, 200.0f }, { 0.0f, 1.0f, 0.0f } },
            { "pitched 45 degrees down", { 40.0f, 0.0f, -30.0f }, { 0.0f, 1.0f, 0.0f } },
            // The view the old [1][1] read turned into "nothing ever splits".
            { "straight down", { 40.0f, 0.0f, -90.0f }, { 0.0f, 0.0f, 1.0f } },
            { "rolled", { 300.0f, 20.0f, 100.0f }, { 0.4f, 0.9f, 0.1f } },
        };
        for (const auto& view : views)
        {
            const glm::mat4 viewProjection = Projection() * glm::lookAt(eye, view.Target, glm::normalize(view.Up));
            EXPECT_NEAR(TerrainLodMetric::QuadProjectionScale(viewProjection, kViewportHeight), expected, expected * 1.0e-4f)
                << view.Name;
        }
    }

    // RHI::AdjustProjectionForBackend negates the Y row for Vulkan. A scale
    // read as a signed element went negative there, and a negative error is
    // below every threshold.
    TEST(TerrainLodMetric, TheScaleSurvivesAFlippedProjection)
    {
        glm::mat4 flipped = Projection();
        flipped[1][1] *= -1.0f;
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(50.0f, 0.0f, 80.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        EXPECT_NEAR(TerrainLodMetric::QuadProjectionScale(flipped * view, kViewportHeight), ExpectedScale(),
                    ExpectedScale() * 1.0e-4f);
    }

    // Worked through by hand: a 26.25 m leaf of a 420 m, 1024 px terrain has
    // quads of 26.25 / 64 = 0.41 m. At 150 m a pixel is 150 * 2 tan(30 deg) /
    // 1080 = 0.160 m, so a quad is 2.56 px: under a 6 px target the leaf's
    // parent is plenty. The old metric called the same leaf 164 px.
    TEST(TerrainLodMetric, ANodesErrorIsOneOfItsQuadsInPixels)
    {
        const glm::mat4 viewProjection = Projection() * glm::lookAt(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f, 3.0f, 100.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const f32 scale = TerrainLodMetric::QuadProjectionScale(viewProjection, kViewportHeight);
        const f32 leaf = 420.0f / 16.0f;
        EXPECT_NEAR(leaf * scale / 150.0f, 2.557f, 0.01f);
        // Twice the node, or half the distance, is twice the pixels.
        EXPECT_NEAR((2.0f * leaf) * scale / 150.0f, 2.0f * 2.557f, 0.02f);
        EXPECT_NEAR(leaf * scale / 75.0f, 2.0f * 2.557f, 0.02f);
    }

    TEST(TerrainLodMetric, ADegenerateViewSplitsNothing)
    {
        EXPECT_EQ(TerrainLodMetric::QuadProjectionScale(glm::mat4(0.0f), kViewportHeight), 0.0f);
        EXPECT_EQ(TerrainLodMetric::QuadProjectionScale(Projection(), 0.0f), 0.0f);
        EXPECT_EQ(TerrainLodMetric::QuadProjectionScale(Projection(), std::numeric_limits<f32>::quiet_NaN()), 0.0f);
        glm::mat4 broken = Projection();
        broken[1][1] = std::numeric_limits<f32>::infinity();
        EXPECT_EQ(TerrainLodMetric::QuadProjectionScale(broken, kViewportHeight), 0.0f);
    }
} // namespace OloEngine::Tests
