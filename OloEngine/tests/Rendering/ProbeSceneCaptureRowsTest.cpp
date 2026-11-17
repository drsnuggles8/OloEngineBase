// OLO_TEST_LAYER: L1
// =============================================================================
// ProbeSceneCapture::NormalizeFaceRows -- the row order a probe capture hands
// its bakers, on the CPU and without a device.
//
// A probe capture reads each cube face back from a render target and promises
// its sink the rows BOTTOM-UP on every backend: the face orientations are
// chosen for that order, the reflection baker uploads the texels into a cube
// map face as they come and the light-probe baker maps a row to a direction.
// An OpenGL target stores its rows bottom-up and a Vulkan one top-down, so the
// capture turns a Vulkan readback over. Left out, every side face baked on
// Vulkan is upside down; applied on OpenGL, the same on that backend.
//
// The device tests that measure the result on both backends
// (LightProbeRasterBakeTest) skip where there is no GPU. This one runs
// everywhere.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ProbeSceneCapture.h"

#include <gtest/gtest.h>

#include <span>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kResolution = 4u;

        // Every float distinct and readable: row * 100 + column * 10 + channel.
        // No row is the mirror of another and no texel the mirror of another in
        // its row, so reversing the rows, the columns or the channels each
        // gives a different face.
        [[nodiscard]] std::vector<f32> FaceWithDistinctRows(u32 resolution)
        {
            std::vector<f32> face(static_cast<sizet>(resolution) * resolution * 4u);
            for (u32 row = 0; row < resolution; ++row)
            {
                for (u32 column = 0; column < resolution; ++column)
                {
                    for (u32 channel = 0; channel < 4u; ++channel)
                    {
                        face[(static_cast<sizet>(row) * resolution + column) * 4u + channel] =
                            static_cast<f32>(row * 100u + column * 10u + channel);
                    }
                }
            }
            return face;
        }

        [[nodiscard]] f32 At(const std::vector<f32>& face, u32 resolution, u32 row, u32 column, u32 channel)
        {
            return face[(static_cast<sizet>(row) * resolution + column) * 4u + channel];
        }
    } // namespace

    TEST(ProbeSceneCaptureRows, ABottomUpReadbackIsHandedOverAsItCame)
    {
        const std::vector<f32> readback = FaceWithDistinctRows(kResolution);
        std::vector<f32> face = readback;
        ProbeSceneCapture::NormalizeFaceRows(face, kResolution, /*rowsAreBottomUp=*/true);
        EXPECT_EQ(face, readback) << "an OpenGL readback is already in the promised order and must not be touched";
    }

    TEST(ProbeSceneCaptureRows, ATopDownReadbackHasItsRowsReversedAndNothingElse)
    {
        const std::vector<f32> readback = FaceWithDistinctRows(kResolution);
        std::vector<f32> face = readback;
        ProbeSceneCapture::NormalizeFaceRows(face, kResolution, /*rowsAreBottomUp=*/false);
        ASSERT_NE(face, readback) << "a Vulkan readback was handed over top-down: every side face bakes upside down";
        for (u32 row = 0; row < kResolution; ++row)
        {
            for (u32 column = 0; column < kResolution; ++column)
            {
                for (u32 channel = 0; channel < 4u; ++channel)
                {
                    // f32 values that are exact small integers: equality is the contract.
                    EXPECT_FLOAT_EQ(At(face, kResolution, row, column, channel),
                                    At(readback, kResolution, kResolution - 1u - row, column, channel))
                        << "row " << row << ", column " << column << ", channel " << channel
                        << ": only the rows may move, a row's texels and a texel's channels stay in place";
                }
            }
        }
    }

    TEST(ProbeSceneCaptureRows, AnOddRowCountKeepsItsMiddleRowAndTwiceOverIsTheReadback)
    {
        constexpr u32 kOdd = 5u;
        const std::vector<f32> readback = FaceWithDistinctRows(kOdd);
        std::vector<f32> face = readback;
        ProbeSceneCapture::NormalizeFaceRows(face, kOdd, /*rowsAreBottomUp=*/false);
        for (u32 column = 0; column < kOdd; ++column)
        {
            EXPECT_FLOAT_EQ(At(face, kOdd, 2u, column, 1u), At(readback, kOdd, 2u, column, 1u)) << "the middle row moved";
            EXPECT_FLOAT_EQ(At(face, kOdd, 0u, column, 1u), At(readback, kOdd, 4u, column, 1u));
        }
        ProbeSceneCapture::NormalizeFaceRows(face, kOdd, /*rowsAreBottomUp=*/false);
        EXPECT_EQ(face, readback) << "reversing the rows twice is not the identity: rows were lost or duplicated";
    }

    TEST(ProbeSceneCaptureRows, ASpanThatIsNotAWholeFaceIsLeftAlone)
    {
        const std::vector<f32> readback = FaceWithDistinctRows(kResolution);
        std::vector<f32> face = readback;
        // One row short of a face, and a resolution the span does not hold.
        ProbeSceneCapture::NormalizeFaceRows(std::span<f32>(face.data(), face.size() - static_cast<sizet>(kResolution) * 4u),
                                             kResolution, /*rowsAreBottomUp=*/false);
        EXPECT_EQ(face, readback) << "a short span was reversed as if it were a whole face";
        ProbeSceneCapture::NormalizeFaceRows(face, kResolution * 2u, /*rowsAreBottomUp=*/false);
        EXPECT_EQ(face, readback) << "rows were swapped past the end of the span";
        std::vector<f32> empty;
        ProbeSceneCapture::NormalizeFaceRows(empty, 0u, /*rowsAreBottomUp=*/false);
        EXPECT_TRUE(empty.empty());
    }
} // namespace OloEngine::Tests
