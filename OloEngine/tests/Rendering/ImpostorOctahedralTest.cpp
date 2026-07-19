// =============================================================================
// ImpostorOctahedralTest.cpp
//
// Contract tests for the octahedral impostor atlas mapping (issue #433) — the
// CPU library OloEngine/src/OloEngine/Renderer/Impostor/OctahedralImpostor.h,
// which is mirrored expression-for-expression by the GLSL include
// OloEditor/assets/shaders/include/OctahedralImpostor.glsl and the impostor
// card/bake shaders. These pin the encode/decode round-trip, the frame<->grid
// mapping, the 3-tile barycentric blend, and the LOD cross-fade — the subtle
// math whose failure mode is a silently wrong distant billboard.
//
// Pure math, no GL context required.
//
// OLO_TEST_LAYER: unit
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Impostor/OctahedralImpostor.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    using namespace OloEngine::Impostor;

    namespace
    {
        // A spread of unit directions on the upper hemisphere. Elevation starts
        // at 5deg (not 0) to stay clear of the horizon, where DirectionToOctaHemi
        // deliberately clamps y to 0.001 (the hemi layout cannot represent y<0) —
        // that clamp is correct behaviour, not a round-trip we want to assert.
        std::vector<glm::vec3> UpperHemisphereDirs()
        {
            std::vector<glm::vec3> dirs;
            for (i32 ei = 1; ei <= 17; ++ei) // elevation 5..85 deg
            {
                const f32 elev = glm::radians(static_cast<f32>(ei) * 5.0f);
                for (i32 ai = 0; ai < 16; ++ai) // azimuth (includes axis-aligned az=0/90/180/270)
                {
                    const f32 az = glm::radians(static_cast<f32>(ai) * 22.5f);
                    glm::vec3 d(std::cos(elev) * std::cos(az), std::sin(elev), std::cos(elev) * std::sin(az));
                    dirs.push_back(glm::normalize(d));
                }
            }
            return dirs;
        }
    } // namespace

    // ── Encode/decode are exact inverses ──────────────────────────────────

    TEST(ImpostorOctahedralTest, HemiEncodeDecodeRoundTrip)
    {
        for (const glm::vec3& dir : UpperHemisphereDirs())
        {
            const glm::vec2 oct = DirectionToOctaHemi(dir);
            // Encode maps into [-1, 1]^2.
            EXPECT_GE(oct.x, -1.001f);
            EXPECT_LE(oct.x, 1.001f);
            EXPECT_GE(oct.y, -1.001f);
            EXPECT_LE(oct.y, 1.001f);

            const glm::vec3 back = OctaHemiToDirection(oct);
            EXPECT_NEAR(back.x, dir.x, 1e-4f);
            EXPECT_NEAR(back.y, dir.y, 1e-4f);
            EXPECT_NEAR(back.z, dir.z, 1e-4f);
        }
    }

    TEST(ImpostorOctahedralTest, SphereEncodeDecodeRoundTrip)
    {
        // Full sphere: sample both hemispheres.
        for (i32 ei = -8; ei <= 8; ++ei)
        {
            const f32 elev = glm::radians(static_cast<f32>(ei) * 10.0f);
            for (i32 ai = 0; ai < 16; ++ai)
            {
                const f32 az = glm::radians(static_cast<f32>(ai) * 22.5f);
                glm::vec3 dir = glm::normalize(glm::vec3(std::cos(elev) * std::cos(az), std::sin(elev), std::cos(elev) * std::sin(az)));
                const glm::vec2 oct = DirectionToOctaSphere(dir);
                const glm::vec3 back = OctaSphereToDirection(oct);
                EXPECT_NEAR(back.x, dir.x, 1e-4f);
                EXPECT_NEAR(back.y, dir.y, 1e-4f);
                EXPECT_NEAR(back.z, dir.z, 1e-4f);
            }
        }
    }

    // ── Frame <-> grid consistency ────────────────────────────────────────

    TEST(ImpostorOctahedralTest, FrameToDirectionLandsOnItsOwnGridCell)
    {
        // Interior frames only. The octahedral square's boundary is inherently
        // degenerate: for the full sphere all four corners collapse to the -Y
        // pole (many-to-one), and for the hemi layout the edge frames sit on the
        // horizon where the y-clamp perturbs re-encoding. Both are correct map
        // properties, not round-trip failures — the interior (which is what the
        // runtime samples for any non-grazing view) is a clean bijection.
        constexpr u32 N = 8;
        for (bool hemi : { true, false })
        {
            for (u32 fy = 1; fy + 1 < N; ++fy)
            {
                for (u32 fx = 1; fx + 1 < N; ++fx)
                {
                    const glm::ivec2 frame(static_cast<i32>(fx), static_cast<i32>(fy));
                    const glm::vec3 dir = FrameToDirection(frame, N, hemi);
                    const glm::vec2 grid = DirectionToGrid(dir, N, hemi);
                    // The direction a frame was captured from must map back onto
                    // that exact frame's grid coordinate.
                    EXPECT_NEAR(grid.x, static_cast<f32>(fx), 5e-3f) << "hemi=" << hemi << " frame=(" << fx << "," << fy << ")";
                    EXPECT_NEAR(grid.y, static_cast<f32>(fy), 5e-3f) << "hemi=" << hemi << " frame=(" << fx << "," << fy << ")";
                }
            }
        }
    }

    TEST(ImpostorOctahedralTest, HemiTopFrameIsStraightUp)
    {
        // The hemi-octahedron centre (grid centre) captures the straight-up view.
        constexpr u32 N = 9; // odd so a centre vertex exists
        const glm::ivec2 centre(static_cast<i32>(N / 2), static_cast<i32>(N / 2));
        const glm::vec3 dir = FrameToDirection(centre, N, /*hemi=*/true);
        EXPECT_NEAR(dir.y, 1.0f, 1e-3f);
    }

    // ── 3-tile barycentric blend ──────────────────────────────────────────

    TEST(ImpostorOctahedralTest, TileBlendWeightsSumToOneAndFramesInRange)
    {
        constexpr u32 N = 8;
        const f32 maxIndex = static_cast<f32>(N - 1);
        for (f32 gy = 0.0f; gy <= maxIndex + 0.001f; gy += 0.37f)
        {
            for (f32 gx = 0.0f; gx <= maxIndex + 0.001f; gx += 0.29f)
            {
                const TileBlend b = ComputeTileBlend(glm::vec2(gx, gy), N);
                const f32 sum = b.Weights[0] + b.Weights[1] + b.Weights[2];
                EXPECT_NEAR(sum, 1.0f, 1e-5f) << "grid=(" << gx << "," << gy << ")";
                for (i32 i = 0; i < 3; ++i)
                {
                    EXPECT_GE(b.Weights[i], -1e-6f);
                    EXPECT_LE(b.Weights[i], 1.0f + 1e-6f);
                    EXPECT_GE(b.Frames[i].x, 0);
                    EXPECT_GE(b.Frames[i].y, 0);
                    EXPECT_LE(b.Frames[i].x, static_cast<i32>(N - 1));
                    EXPECT_LE(b.Frames[i].y, static_cast<i32>(N - 1));
                }
            }
        }
    }

    TEST(ImpostorOctahedralTest, TileBlendAtVertexIsSingleFrame)
    {
        constexpr u32 N = 8;
        // Exactly on a lattice vertex -> weight fully on that frame.
        const TileBlend b = ComputeTileBlend(glm::vec2(3.0f, 5.0f), N);
        EXPECT_NEAR(b.Weights[0], 1.0f, 1e-5f);
        EXPECT_EQ(b.Frames[0], glm::ivec2(3, 5));
    }

    TEST(ImpostorOctahedralTest, TileBlendReconstructsGridCoordinate)
    {
        // The weighted sum of the 3 frame coordinates should equal the sample
        // point — the definition of a barycentric blend over the cell corners.
        constexpr u32 N = 16;
        const glm::vec2 grid(4.3f, 9.7f);
        const TileBlend b = ComputeTileBlend(grid, N);
        glm::vec2 recon(0.0f);
        for (i32 i = 0; i < 3; ++i)
            recon += glm::vec2(b.Frames[i]) * b.Weights[i];
        EXPECT_NEAR(recon.x, grid.x, 1e-4f);
        EXPECT_NEAR(recon.y, grid.y, 1e-4f);
    }

    // ── LOD cross-fade ────────────────────────────────────────────────────

    TEST(ImpostorOctahedralTest, ImpostorFadeRampsMonotonically)
    {
        constexpr f32 start = 40.0f;
        constexpr f32 band = 15.0f;
        EXPECT_FLOAT_EQ(ImpostorFade(0.0f, start, band), 0.0f);
        EXPECT_FLOAT_EQ(ImpostorFade(start, start, band), 0.0f);
        EXPECT_FLOAT_EQ(ImpostorFade(start + band, start, band), 1.0f);
        EXPECT_FLOAT_EQ(ImpostorFade(1000.0f, start, band), 1.0f);

        const f32 mid = ImpostorFade(start + band * 0.5f, start, band);
        EXPECT_NEAR(mid, 0.5f, 1e-5f);

        // Monotonic non-decreasing across the band.
        f32 prev = -1.0f;
        for (f32 d = 0.0f; d <= 80.0f; d += 2.0f)
        {
            const f32 v = ImpostorFade(d, start, band);
            EXPECT_GE(v, prev - 1e-6f);
            prev = v;
        }
    }

    TEST(ImpostorOctahedralTest, ImpostorFadeZeroBandIsHardSwitch)
    {
        EXPECT_FLOAT_EQ(ImpostorFade(39.9f, 40.0f, 0.0f), 0.0f);
        EXPECT_FLOAT_EQ(ImpostorFade(40.1f, 40.0f, 0.0f), 1.0f);
    }
} // namespace OloEngine::Tests
