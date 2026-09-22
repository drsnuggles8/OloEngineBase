// =============================================================================
// FoliageAlphaCoverageContractTest.cpp
//
// The arithmetic behind the #1399 diagnostic: a foliage layer's albedo can pass
// almost nothing at its AlphaCutoff, and every other signal in the engine still
// reports success. What is pinned here, on synthetic images so every expected
// number is exact:
//
//   1. The pass fraction is the SHADER's comparison — `alpha < cutoff`
//      discards — evaluated per UNORM byte, including a texel exactly at the
//      cutoff and a NaN cutoff.
//   2. The verdict depends on the ROLE. The same 18% is idiomatic on a grass
//      card and a see-through tree on an authored mesh; a single threshold
//      would get one of them wrong.
//   3. Mesh coverage is measured over the SURFACE, area-weighted, not over the
//      sheet — and the texel lookup uses the engine's v-up convention.
//   4. The warning names the layer, texture, cutoff and fraction, and says it
//      changed nothing.
//
// The GL half — a real layer through FoliageRenderer, warning or staying
// quiet — is FoliageAlphaCoverageWarningTest.
//
// OLO_TEST_LAYER: L1
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Terrain/Foliage/FoliageAlphaCoverage.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace AC = FoliageAlphaCoverage;

        // `alphas` is TOP-DOWN, row by row — the order an image file stores.
        [[nodiscard]] AC::AlphaPlane MakePlane(u32 width, u32 height, const std::vector<u8>& alphas)
        {
            AC::AlphaPlane plane;
            plane.Width = width;
            plane.Height = height;
            for (const u8 a : alphas)
                plane.Alpha.Add(a);
            return plane;
        }

        // A histogram with `passing` of `total` texels at 255 and the rest at 0.
        [[nodiscard]] AC::Histogram MakeCoverage(u32 passing, u32 total)
        {
            AC::Histogram histogram;
            histogram.Add(255, static_cast<f64>(passing));
            histogram.Add(0, static_cast<f64>(total - passing));
            return histogram;
        }

        [[nodiscard]] Vertex MakeVertex(glm::vec3 position, glm::vec2 uv)
        {
            return Vertex(position, glm::vec3(0.0f, 0.0f, 1.0f), uv);
        }
    } // namespace

    // ── 1. The shader's comparison ──────────────────────────────────────────

    TEST(FoliageAlphaCoverage, PassFractionIsTheShadersAlphaTestPerByte)
    {
        AC::Histogram histogram;
        histogram.Add(0);
        histogram.Add(64);
        histogram.Add(128);
        histogram.Add(255);

        EXPECT_FLOAT_EQ(histogram.PassFraction(0.0f), 1.0f) << "cutoff 0 discards nothing";
        EXPECT_FLOAT_EQ(histogram.PassFraction(0.3f), 0.5f) << "64/255 = 0.251 is discarded, 128/255 kept";
        EXPECT_FLOAT_EQ(histogram.PassFraction(1.0f), 0.25f) << "only a fully opaque texel survives cutoff 1";
        EXPECT_FLOAT_EQ(histogram.PassFraction(1.01f), 0.0f);
    }

    TEST(FoliageAlphaCoverage, ATexelExactlyAtTheCutoffPasses)
    {
        // `alpha < cutoff` discards, so equality KEEPS the texel. An
        // implementation that wrote `alpha > cutoff` would drop every texel
        // sitting on the boundary, which for a hand-painted mask is a lot.
        AC::Histogram histogram;
        histogram.Add(128);
        EXPECT_FLOAT_EQ(histogram.PassFraction(128.0f / 255.0f), 1.0f);
        EXPECT_FLOAT_EQ(histogram.PassFraction(std::nextafter(128.0f / 255.0f, 1.0f)), 0.0f);
    }

    TEST(FoliageAlphaCoverage, ANaNCutoffPassesEverythingAsTheShaderDoes)
    {
        // `alpha < NaN` is false, so the shader discards nothing. Reporting 0%
        // here would warn about a layer that is in fact drawn solid.
        const AC::Histogram histogram = MakeCoverage(1, 10);
        EXPECT_FLOAT_EQ(histogram.PassFraction(std::numeric_limits<f32>::quiet_NaN()), 1.0f);
    }

    TEST(FoliageAlphaCoverage, AnEmptyHistogramPassesNothingAndIsReportedEmpty)
    {
        const AC::Histogram histogram;
        EXPECT_TRUE(histogram.IsEmpty());
        EXPECT_FLOAT_EQ(histogram.PassFraction(0.5f), 0.0f);
    }

    TEST(FoliageAlphaCoverage, WeightsAreFractionsOfTheMeasuredTotal)
    {
        AC::Histogram histogram;
        histogram.Add(255, 3.0);
        histogram.Add(0, 1.0);
        EXPECT_FLOAT_EQ(histogram.PassFraction(0.5f), 0.75f);
    }

    // ── 2. The verdict depends on the role ──────────────────────────────────

    TEST(FoliageAlphaCoverage, TheSameFractionIsIdiomaticOnACardAndWrongOnAMesh)
    {
        // The issue's own number: grass.png passed 17.9% at 0.3. On a grass
        // card that is exactly right; mapped onto a tree it is a see-through
        // tree. This is the whole reason the threshold is per role.
        EXPECT_EQ(AC::Judge(AC::Role::Card, 0.179f), AC::Verdict::Plausible);
        EXPECT_EQ(AC::Judge(AC::Role::AuthoredMesh, 0.179f), AC::Verdict::TooSparse);
        EXPECT_EQ(AC::Judge(AC::Role::ImpostorBake, 0.179f), AC::Verdict::TooSparse);
    }

    TEST(FoliageAlphaCoverage, ACardWarnsOnlyWhenNearlyEmptyOrNearlySolid)
    {
        // The shipped cards span 6.1% (dry grass) to 40.5% (broadleaf): all
        // quiet. Both ends of the band are defects with a known cause.
        for (const f32 shipped : { 0.061f, 0.104f, 0.12f, 0.287f, 0.405f })
            EXPECT_EQ(AC::Judge(AC::Role::Card, shipped), AC::Verdict::Plausible) << shipped;
        EXPECT_EQ(AC::Judge(AC::Role::Card, 0.01f), AC::Verdict::TooSparse);
        EXPECT_EQ(AC::Judge(AC::Role::Card, 0.0f), AC::Verdict::TooSparse);
        EXPECT_EQ(AC::Judge(AC::Role::Card, 0.97f), AC::Verdict::TooSolid)
            << "a card this opaque is a solid rectangle: a source atlas or an image with no alpha";
        EXPECT_EQ(AC::Judge(AC::Role::Card, 1.0f), AC::Verdict::TooSolid);
    }

    TEST(FoliageAlphaCoverage, AMeshHasAFloorAndNoCeiling)
    {
        // Shipped foliage parts, as the engine samples them: broadleaf 44.8%,
        // fern 52.5%, pine 54.4%, dry grass 87.6%, shrub 92.8%; trunks and
        // bark are fully opaque. The stand-in grass-on-pine the issue was found
        // on passes 19.4% of the pine's surface, and so does an impostor baked
        // from it.
        for (const f32 shipped : { 0.448f, 0.525f, 0.544f, 0.876f, 0.928f, 1.0f })
        {
            EXPECT_EQ(AC::Judge(AC::Role::AuthoredMesh, shipped), AC::Verdict::Plausible) << shipped;
            EXPECT_EQ(AC::Judge(AC::Role::ImpostorBake, shipped), AC::Verdict::Plausible) << shipped;
        }
        EXPECT_EQ(AC::Judge(AC::Role::AuthoredMesh, 0.194f), AC::Verdict::TooSparse);
        EXPECT_EQ(AC::Judge(AC::Role::ImpostorBake, 0.194f), AC::Verdict::TooSparse);
    }

    TEST(FoliageAlphaCoverage, BandEdgesAreInclusive)
    {
        for (const AC::Role role : { AC::Role::Card, AC::Role::AuthoredMesh, AC::Role::ImpostorBake })
        {
            const AC::Band band = AC::PlausibleBand(role);
            EXPECT_LT(band.Min, band.Max);
            EXPECT_EQ(AC::Judge(role, band.Min), AC::Verdict::Plausible) << AC::RoleName(role);
            EXPECT_EQ(AC::Judge(role, band.Max), AC::Verdict::Plausible) << AC::RoleName(role);
        }
    }

    // ── 3. Surface, not sheet ───────────────────────────────────────────────

    TEST(FoliageAlphaCoverage, MeasureSheetCountsEveryTexelOnce)
    {
        const AC::AlphaPlane plane = MakePlane(2, 2, { 255, 0, 0, 0 });
        const AC::Histogram histogram = AC::MeasureSheet(plane);
        EXPECT_DOUBLE_EQ(histogram.Total, 4.0);
        EXPECT_FLOAT_EQ(histogram.PassFraction(0.5f), 0.25f);
    }

    TEST(FoliageAlphaCoverage, VRunsUpTheImageAsItDoesAfterTexture2DsFlip)
    {
        // Top row transparent, bottom row opaque (file order). Texture2D flips
        // on load, so v = 0 is the BOTTOM of the file: sampling low v must hit
        // the opaque row. A plane read the other way up measures the wrong
        // half of every atlas — the #1398 postmortem's brown pine cones.
        const AC::AlphaPlane plane = MakePlane(1, 2, { 0, 255 });
        EXPECT_EQ(plane.AlphaAt({ 0.5f, 0.25f }), 255);
        EXPECT_EQ(plane.AlphaAt({ 0.5f, 0.75f }), 0);
        // REPEAT wrap, on both axes and for negative coordinates.
        EXPECT_EQ(plane.AlphaAt({ 1.5f, 1.25f }), 255);
        EXPECT_EQ(plane.AlphaAt({ -0.5f, -0.25f }), 0);
    }

    TEST(FoliageAlphaCoverage, SurfaceSamplesAreWeightedByWorldArea)
    {
        // Two triangles over disjoint halves of UV space. The one mapped onto
        // the OPAQUE left half has three times the world area, so 75% of the
        // surface passes — while the sheet, measured flat, is exactly 50%.
        // That gap is the whole argument for measuring the surface.
        const std::vector<Vertex> vertices = {
            // big: 3 x 2 right triangle, UVs in the left half
            MakeVertex({ 0.0f, 0.0f, 0.0f }, { 0.05f, 0.05f }),
            MakeVertex({ 3.0f, 0.0f, 0.0f }, { 0.45f, 0.05f }),
            MakeVertex({ 0.0f, 2.0f, 0.0f }, { 0.05f, 0.95f }),
            // small: 1 x 2, UVs in the right half
            MakeVertex({ 10.0f, 0.0f, 0.0f }, { 0.55f, 0.05f }),
            MakeVertex({ 11.0f, 0.0f, 0.0f }, { 0.95f, 0.05f }),
            MakeVertex({ 10.0f, 2.0f, 0.0f }, { 0.55f, 0.95f }),
        };
        const std::vector<u32> indices = { 0, 1, 2, 3, 4, 5 };
        const AC::AlphaPlane plane = MakePlane(2, 1, { 255, 0 });

        const TArray<glm::vec2> uvs = AC::SampleSurfaceUVs(vertices, indices);
        ASSERT_EQ(uvs.Num(), static_cast<i32>(AC::kDefaultSurfaceSamples));
        const AC::Histogram surface = AC::MeasureAtUVs(plane, { uvs.GetData(), static_cast<sizet>(uvs.Num()) });

        EXPECT_NEAR(surface.PassFraction(0.5f), 0.75f, 0.005f);
        EXPECT_FLOAT_EQ(AC::MeasureSheet(plane).PassFraction(0.5f), 0.5f);
    }

    TEST(FoliageAlphaCoverage, SurfaceSamplesAreDeterministic)
    {
        // The same mesh must give the same figure on every run and platform,
        // or a layer near a band edge would warn intermittently.
        const std::vector<Vertex> vertices = {
            MakeVertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f }),
            MakeVertex({ 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }),
            MakeVertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }),
        };
        const std::vector<u32> indices = { 0, 1, 2 };
        const TArray<glm::vec2> first = AC::SampleSurfaceUVs(vertices, indices, 64);
        const TArray<glm::vec2> second = AC::SampleSurfaceUVs(vertices, indices, 64);
        ASSERT_EQ(first.Num(), 64);
        ASSERT_EQ(second.Num(), 64);
        for (i32 i = 0; i < first.Num(); ++i)
        {
            EXPECT_TRUE(Math::BitwiseEqual(first[i], second[i])) << "bitwise identity is the property: " << i;
            // Inside the triangle u + v <= 1, so every sample landed on it.
            EXPECT_LE(first[i].x + first[i].y, 1.0f + 1e-5f) << i;
        }
    }

    TEST(FoliageAlphaCoverage, ASurfaceWithNoAreaYieldsNoSamples)
    {
        // Degenerate and out-of-range triangles are skipped; with nothing left
        // there is nothing to measure, and the caller must say so rather than
        // report 0%.
        const std::vector<Vertex> vertices = {
            MakeVertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f }),
            MakeVertex({ 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }),
            MakeVertex({ 2.0f, 0.0f, 0.0f }, { 0.0f, 1.0f }), // collinear
        };
        EXPECT_TRUE(AC::SampleSurfaceUVs(vertices, std::vector<u32>{ 0, 1, 2 }).IsEmpty());
        EXPECT_TRUE(AC::SampleSurfaceUVs(vertices, std::vector<u32>{ 0, 1, 7 }).IsEmpty());
        EXPECT_TRUE(AC::SampleSurfaceUVs({}, std::vector<u32>{ 0, 1, 2 }).IsEmpty());
    }

    // ── Decoding: what Texture2D would sample ───────────────────────────────

    TEST(FoliageAlphaCoverage, DecodeKeepsAFourChannelAlphaTopDown)
    {
        // Row 0 (top) transparent, row 1 opaque.
        const u8 rgba[] = { 10, 20, 30, 0, 10, 20, 30, 255 };
        const auto path = TempFile("alpha_rgba.png");
        ASSERT_NE(stbi_write_png(path.string().c_str(), 1, 2, 4, rgba, 4), 0);

        AC::AlphaPlane plane;
        std::string error;
        ASSERT_TRUE(AC::DecodeAlpha(path, plane, error)) << error;
        ASSERT_EQ(plane.Width, 1u);
        ASSERT_EQ(plane.Height, 2u);
        EXPECT_EQ(plane.Alpha[0], 0);
        EXPECT_EQ(plane.Alpha[1], 255);
        EXPECT_EQ(plane.AlphaAt({ 0.5f, 0.25f }), 255) << "v = 0 is the bottom row once uploaded";
    }

    TEST(FoliageAlphaCoverage, AnImageWithoutAlphaIsFullyOpaque)
    {
        // RGB8 samples alpha as 1. A card textured with it is a solid square,
        // which the card band then reports.
        const u8 rgb[] = { 0, 0, 0, 0, 0, 0 };
        const auto path = TempFile("no_alpha.png");
        ASSERT_NE(stbi_write_png(path.string().c_str(), 2, 1, 3, rgb, 6), 0);

        AC::AlphaPlane plane;
        std::string error;
        ASSERT_TRUE(AC::DecodeAlpha(path, plane, error)) << error;
        const f32 fraction = AC::MeasureSheet(plane).PassFraction(0.5f);
        EXPECT_FLOAT_EQ(fraction, 1.0f);
        EXPECT_EQ(AC::Judge(AC::Role::Card, fraction), AC::Verdict::TooSolid);
    }

    TEST(FoliageAlphaCoverage, AnUndecodableFileIsAnErrorNotAZeroPercentTexture)
    {
        AC::AlphaPlane plane;
        std::string error;
        EXPECT_FALSE(AC::DecodeAlpha(TempFile("does_not_exist.png"), plane, error));
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(plane.IsEmpty());
    }

    // ── 4. The warning text ─────────────────────────────────────────────────

    TEST(FoliageAlphaCoverage, TheWarningNamesEverythingNeededToFixIt)
    {
        AC::Entry entry;
        entry.Kind = AC::Role::AuthoredMesh;
        entry.Texture = "assets/textures/grass.png";
        entry.Surface = "part 0 of 'pine.obj'";
        entry.Coverage = MakeCoverage(179, 1000);
        entry.Measured = true;

        const std::string text = AC::Describe(entry, "Woodland Pines", 0.3f);
        EXPECT_NE(text.find("'Woodland Pines'"), std::string::npos) << text;
        EXPECT_NE(text.find("assets/textures/grass.png"), std::string::npos) << text;
        EXPECT_NE(text.find("part 0 of 'pine.obj'"), std::string::npos) << text;
        EXPECT_NE(text.find("17.9%"), std::string::npos) << text;
        EXPECT_NE(text.find("AlphaCutoff 0.30"), std::string::npos) << text;
        EXPECT_NE(text.find("30%"), std::string::npos) << "the floor it fell below: " << text;
        EXPECT_NE(text.find("Diagnostic only"), std::string::npos) << text;
    }

    TEST(FoliageAlphaCoverage, EachVerdictSaysWhatTheLikelyMistakeIs)
    {
        AC::Entry card;
        card.Kind = AC::Role::Card;
        card.Texture = "atlas.png";
        card.Measured = true;

        card.Coverage = MakeCoverage(1, 1000);
        EXPECT_NE(AC::Describe(card, "L", 0.5f).find("stray pixels"), std::string::npos);
        card.Coverage = MakeCoverage(990, 1000);
        EXPECT_NE(AC::Describe(card, "L", 0.5f).find("solid rectangle"), std::string::npos);

        AC::Entry impostor = card;
        impostor.Kind = AC::Role::ImpostorBake;
        impostor.Surface = "part 0 of 'pine.obj'";
        impostor.Coverage = MakeCoverage(192, 1000);
        const std::string text = AC::Describe(impostor, "L", 0.3f);
        EXPECT_NE(text.find("bakes part 0 of 'pine.obj' into its impostor"), std::string::npos) << text;
        EXPECT_NE(text.find("19.2%"), std::string::npos) << text;
    }

    TEST(FoliageAlphaCoverage, APlausibleOrUnmeasuredEntryProducesNoWarning)
    {
        AC::Entry entry;
        entry.Kind = AC::Role::Card;
        entry.Coverage = MakeCoverage(18, 100);
        entry.Measured = true;
        EXPECT_TRUE(AC::Describe(entry, "Meadow", 0.5f).empty());

        // Unmeasured is silent HERE because the renderer has already said so
        // when the decode failed; judging an empty histogram would report 0%.
        entry.Coverage = AC::Histogram{};
        entry.Measured = false;
        EXPECT_TRUE(AC::Describe(entry, "Meadow", 0.5f).empty());
    }
} // namespace OloEngine::Tests
