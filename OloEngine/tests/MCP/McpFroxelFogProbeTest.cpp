// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure froxel <-> world mapping behind olo_froxel_fog_probe
// (issue #607; relates to #435).
//
// The tool exists to separate "the scatter pass injected nothing here" from "the
// composite tapped the wrong froxel" — which only works if the probe addresses
// the SAME cell the shader shaded. If the mapping is off by a cell, the tool
// becomes a confident liar: it reports a neighbour's scattering as the sampled
// point's, and an agent chases a bug that is not there.
//
// So these tests are a transcription check against the shader source, not a
// smoke test. The two hazards they pin:
//
//   * The z distribution is EXPONENTIAL (FroxelFogScatter.comp:
//     viewDepth = near * exp2(log2(far/near) * sliceCoord)). A linear inverse
//     would agree with the exponential one at BOTH end slices and be wrong
//     everywhere between — the round-trip test walks every slice for exactly
//     that reason.
//   * A froxel is a frustum cell, so the shader scales the VIEW RAY to the slice
//     depth (viewPos = viewRay * depth / -viewRay.z), not the view axis. Using
//     the axis would be right only down the screen centre and increasingly wrong
//     toward the corners — so the round trip is exercised at the corners too.
#include "MCP/McpFroxelFogProbe.h"

#include <glm/gtc/matrix_transform.hpp>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::MCP::FroxelFog;

    // A volume with the engine's real dimensions and a plausible camera, built
    // the way VolumetricFogPass::Execute builds the UBO it uploads.
    // (nearPlane / farPlane, never `near` / `far` — those are macros from
    // windows.h's windef.h and would not even parse here.)
    Volume MakeVolume(f32 nearPlane = 0.1f, f32 farPlane = 120.0f,
                      const glm::vec3& renderOrigin = glm::vec3(0.0f))
    {
        Volume v;
        v.DimX = 160;
        v.DimY = 90;
        v.DimZ = 64;
        v.Near = nearPlane;
        v.Far = farPlane;
        v.LogFarOverNear = std::log2(farPlane / nearPlane);

        // Render-relative view: an off-axis camera, so a mapping bug cannot hide
        // behind an identity transform.
        v.View = glm::lookAt(glm::vec3(3.0f, 2.0f, 7.0f), glm::vec3(0.0f, 1.0f, 0.0f),
                             glm::vec3(0.0f, 1.0f, 0.0f));
        v.InverseView = glm::inverse(v.View);
        v.Projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
        v.InverseProjection = glm::inverse(v.Projection);
        v.RenderOrigin = renderOrigin;
        return v;
    }

    // The shader's own froxel -> view-depth expression, transcribed literally from
    // FroxelFogScatter.comp (with the temporal jitter at its expectation, 0.5).
    // The core must agree with THIS, not with a re-derivation of it.
    f32 ShaderViewDepth(const Volume& v, i32 z)
    {
        const f32 sliceCoord = (static_cast<f32>(z) + 0.5f) / static_cast<f32>(v.DimZ);
        return v.Near * std::exp2(v.LogFarOverNear * sliceCoord);
    }

    TEST(McpFroxelFogProbe, SliceDepthMatchesTheShadersExponentialDistribution)
    {
        const Volume v = MakeVolume();

        // End points are exact by construction.
        EXPECT_NEAR(SliceViewDepth(v, 0.0f), v.Near, 1e-4f);
        EXPECT_NEAR(SliceViewDepth(v, 1.0f), v.Far, 1e-2f);

        // And every slice centre matches the shader expression.
        for (i32 z = 0; z < v.DimZ; ++z)
        {
            const f32 expected = ShaderViewDepth(v, z);
            const f32 actual = SliceViewDepth(v, (static_cast<f32>(z) + 0.5f) / static_cast<f32>(v.DimZ));
            EXPECT_NEAR(actual, expected, expected * 1e-5f) << "slice " << z;
        }
    }

    TEST(McpFroxelFogProbe, ExponentialSlicingIsNotLinear)
    {
        // Guards the whole point of the exponential mapping: the middle slice sits
        // FAR nearer the camera than a linear split would put it. A linear inverse
        // would pass every end-point test and be badly wrong here.
        const Volume v = MakeVolume(0.1f, 120.0f);
        const f32 middle = SliceViewDepth(v, 0.5f);
        const f32 linearMiddle = 0.5f * (v.Near + v.Far);
        EXPECT_LT(middle, linearMiddle * 0.2f);
        EXPECT_NEAR(middle, std::sqrt(v.Near * v.Far), 1e-3f); // geometric mean
    }

    TEST(McpFroxelFogProbe, SliceCoordInvertsSliceDepth)
    {
        const Volume v = MakeVolume();
        for (i32 z = 0; z < v.DimZ; ++z)
        {
            const f32 t = (static_cast<f32>(z) + 0.5f) / static_cast<f32>(v.DimZ);
            const f32 depth = SliceViewDepth(v, t);
            EXPECT_NEAR(SliceCoordForViewDepth(v, depth), t, 1e-5f) << "slice " << z;
        }
    }

    // THE test: froxel -> world -> froxel must land back in the SAME cell, at
    // every corner of the volume and every depth slice. This is what proves the
    // probe reports the cell the shader shaded.
    TEST(McpFroxelFogProbe, WorldRoundTripReturnsTheSameFroxelAcrossTheWholeVolume)
    {
        const Volume v = MakeVolume();

        for (const i32 x : { 0, 1, 40, 79, 80, 159 })
        {
            for (const i32 y : { 0, 1, 45, 88, 89 })
            {
                for (const i32 z : { 0, 1, 7, 31, 32, 62, 63 })
                {
                    const glm::vec3 world = FroxelCenterWorld(v, x, y, z);
                    const FroxelCoord back = WorldToFroxel(v, world);

                    EXPECT_EQ(back.IX, x) << "froxel (" << x << ", " << y << ", " << z << ")";
                    EXPECT_EQ(back.IY, y) << "froxel (" << x << ", " << y << ", " << z << ")";
                    EXPECT_EQ(back.IZ, z) << "froxel (" << x << ", " << y << ", " << z << ")";
                    EXPECT_FALSE(back.Clamped);
                    EXPECT_TRUE(back.InFrustum);
                    EXPECT_TRUE(back.InDepthRange);

                    // The reported view depth is the slice-centre depth the shader
                    // used, not a re-derived one.
                    EXPECT_NEAR(back.ViewDepth, ShaderViewDepth(v, z), ShaderViewDepth(v, z) * 1e-3f);
                }
            }
        }
    }

    // Camera-relative rendering (issue #429): the fog shaders work in
    // render-RELATIVE space and add the render origin to get absolute world. A
    // probe that forgot the origin would be wrong by exactly the origin — a huge,
    // silent offset in a world-origin-rebased scene.
    TEST(McpFroxelFogProbe, RoundTripSurvivesANonZeroRenderOrigin)
    {
        const glm::vec3 origin(10000.0f, 0.0f, -25000.0f);
        const Volume v = MakeVolume(0.1f, 120.0f, origin);

        const glm::vec3 world = FroxelCenterWorld(v, 100, 20, 40);
        // The absolute position must actually be out by the render origin...
        const Volume atOrigin = MakeVolume(0.1f, 120.0f, glm::vec3(0.0f));
        const glm::vec3 relative = FroxelCenterWorld(atOrigin, 100, 20, 40);
        EXPECT_NEAR(glm::length((world - relative) - origin), 0.0f, 1e-2f);

        // ...and still round-trip.
        const FroxelCoord back = WorldToFroxel(v, world);
        EXPECT_EQ(back.IX, 100);
        EXPECT_EQ(back.IY, 20);
        EXPECT_EQ(back.IZ, 40);
    }

    TEST(McpFroxelFogProbe, PositionsOutsideTheVolumeAreReportedNotSilentlyAnswered)
    {
        const Volume v = MakeVolume(0.1f, 120.0f);

        // Behind the camera: the perspective divide is meaningless; the probe must
        // say "not in the frustum" rather than emit a NaN or a plausible cell.
        const glm::vec3 cameraPos = glm::vec3(v.InverseView[3]) + v.RenderOrigin;
        const glm::vec3 forward = -glm::vec3(v.InverseView[2]);
        const FroxelCoord behind = WorldToFroxel(v, cameraPos - forward * 10.0f);
        EXPECT_FALSE(behind.InDepthRange);
        EXPECT_LT(behind.ViewDepth, 0.0f);
        EXPECT_TRUE(std::isfinite(behind.X));
        EXPECT_TRUE(std::isfinite(behind.Z));

        // Beyond the fog volume's far plane (the volume stops at FogSettings::End,
        // NOT at the camera far plane) — in the frustum, but out of depth range.
        const FroxelCoord tooFar = WorldToFroxel(v, cameraPos + forward * 500.0f);
        EXPECT_TRUE(tooFar.InFrustum);
        EXPECT_FALSE(tooFar.InDepthRange);
        EXPECT_TRUE(tooFar.Clamped);
        EXPECT_EQ(tooFar.IZ, v.DimZ - 1);
    }

    // The cell bounds must be the slice's own extent (FroxelFogIntegrate.comp
    // marches slice z over [D(z/dimZ), D((z+1)/dimZ)]), and must actually contain
    // the cell centre — a bounds box that does not contain its own centre is the
    // kind of thing that makes an agent distrust every number in the response.
    TEST(McpFroxelFogProbe, CellBoundsUseTheSliceExtentAndContainTheCellCenter)
    {
        const Volume v = MakeVolume();

        for (const i32 z : { 0, 17, 63 })
        {
            const CellBounds bounds = FroxelCellBounds(v, 80, 45, z);
            EXPECT_NEAR(bounds.NearViewDepth,
                        SliceViewDepth(v, static_cast<f32>(z) / static_cast<f32>(v.DimZ)), 1e-4f);
            EXPECT_NEAR(bounds.FarViewDepth,
                        SliceViewDepth(v, static_cast<f32>(z + 1) / static_cast<f32>(v.DimZ)), 1e-4f);
            EXPECT_GT(bounds.FarViewDepth, bounds.NearViewDepth);

            const glm::vec3 center = FroxelCenterWorld(v, 80, 45, z);
            constexpr f32 kEps = 1e-3f;
            EXPECT_GE(center.x, bounds.Min.x - kEps);
            EXPECT_GE(center.y, bounds.Min.y - kEps);
            EXPECT_GE(center.z, bounds.Min.z - kEps);
            EXPECT_LE(center.x, bounds.Max.x + kEps);
            EXPECT_LE(center.y, bounds.Max.y + kEps);
            EXPECT_LE(center.z, bounds.Max.z + kEps);
        }
    }

    TEST(McpFroxelFogProbe, JsonCarriesBothVolumesSoScatterAndCompositeCanBeSeparated)
    {
        ProbeResult r;
        r.Vol = MakeVolume();
        r.Coord = WorldToFroxel(r.Vol, FroxelCenterWorld(r.Vol, 12, 34, 20));
        r.Raw.Available = true;
        r.Raw.Value = { 0.25f, 0.5f, 0.75f, 0.02f };
        r.Integrated.Available = false;
        r.Integrated.Unavailable = "integrated volume does not exist this frame.";

        const Json j = ToJson(r);
        EXPECT_EQ(j["froxel"]["coords"][0].get<i32>(), 12);
        EXPECT_EQ(j["froxel"]["coords"][2].get<i32>(), 20);
        EXPECT_TRUE(j["scatter"]["available"].get<bool>());
        EXPECT_FLOAT_EQ(j["scatter"]["extinction"].get<f32>(), 0.02f);
        EXPECT_FLOAT_EQ(j["scatter"]["inScatter"][1].get<f32>(), 0.5f);
        // An unavailable volume degrades with a REASON, never as a zeroed sample
        // that would read as "the scatter pass produced nothing".
        EXPECT_FALSE(j["integrated"]["available"].get<bool>());
        EXPECT_FALSE(j["integrated"].contains("transmittance"));
        EXPECT_FALSE(j["integrated"]["reason"].get<std::string>().empty());
        EXPECT_TRUE(j["froxel"].contains("cellBounds"));
        EXPECT_TRUE(j["volume"].contains("dims"));
    }

    TEST(McpFroxelFogProbe, DegenerateVolumeIsRejected)
    {
        Volume v;
        EXPECT_FALSE(IsUsable(v)); // zero dims / near / far
        v = MakeVolume();
        EXPECT_TRUE(IsUsable(v));
        v.Far = v.Near; // log2(far/near) == 0 -> no depth range at all
        v.LogFarOverNear = 0.0f;
        EXPECT_FALSE(IsUsable(v));
    }
} // namespace
