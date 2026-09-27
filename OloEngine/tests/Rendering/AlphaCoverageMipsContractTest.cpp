// =============================================================================
// AlphaCoverageMipsContractTest.cpp
//
// The CPU half of issues #1441 and #1453: a mip chain for an alpha-TESTED texture
// keeps the fraction of texels that pass the alpha test the same at every level,
// for every cutoff at once. Pinned on synthetic images, so every expected number
// is exact:
//
//   1. The coverage count is the shaders' `alpha < cutoff` comparison per UNORM
//      byte, and only a finite cutoff in (0, 1] marks a texture alpha-tested.
//   2. A plain box chain THINS a sparse cutout (the control), and the
//      histogram-matched chain holds level 0's coverage to within one texel, at
//      0.25, 0.3, 0.5 and 0.75 alike, for a binary card and a soft one.
//   3. The remap only redistributes level 0's alphas: a binary card stays
//      binary, a uniform footprint keeps its value, every aligned 8x8 tile of a
//      kept level holds its footprint's coverage to within a texel of the tile
//      (#1491), and it is deterministic.
//   4. Only alpha differs from the plain chain: colour is bit-identical, and an
//      opaque texture is untouched.
//   5. The cook's gate (IsCutoutAlpha) on each side of its thresholds.
//   6. The box filter itself: extents follow max(1, n / 2), colour is averaged in
//      linear light for an sRGB texture, and an odd extent drops no texel.
//
// The same check on the COOKED chain, after BC7 encode and on the real cards, is
// CookedCutoutCoverageTest. The GPU half — the chain reaching a real texture
// through Texture2D::SetAlphaCoverageCutoff, and surviving a reload — is in
// TextureInPlaceReloadTest.
//
// OLO_TEST_LAYER: L1
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace ACM = AlphaCoverageMips;

        // Sparse cutout: roughly 30% of texels opaque, scattered so no box
        // footprint above level 0 is uniformly opaque — a meadow of one-texel
        // blades, which is exactly what thins.
        [[nodiscard]] std::vector<u8> SparseCutout(u32 width, u32 height)
        {
            std::vector<u8> rgba(static_cast<sizet>(width) * height * 4u);
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    u32 h = x * 73856093u ^ y * 19349663u;
                    h ^= h >> 13;
                    h *= 0x5bd1e995u;
                    h ^= h >> 15;
                    u8* texel = &rgba[(static_cast<sizet>(y) * width + x) * 4u];
                    texel[0] = static_cast<u8>(40u + (h & 0x7Fu));
                    texel[1] = static_cast<u8>(90u + ((h >> 8) & 0x7Fu));
                    texel[2] = static_cast<u8>((h >> 16) & 0x3Fu);
                    texel[3] = (h % 100u) < 30u ? 255u : 0u;
                }
            }
            return rgba;
        }

        [[nodiscard]] std::span<const u8> LevelBytes(const ACM::Chain& chain, i32 index)
        {
            const ACM::Level& level = chain.Levels[index];
            return { chain.Bytes.GetData() + level.Offset, static_cast<sizet>(level.Width) * level.Height * 4u };
        }

        [[nodiscard]] f32 OneTexel(const ACM::Level& level)
        {
            return 1.0f / static_cast<f32>(level.Width * level.Height);
        }
    } // namespace

    TEST(AlphaCoverageMips, OnlyAFiniteCutoffInsideTheUnitIntervalMarksAlphaTesting)
    {
        EXPECT_FLOAT_EQ(ACM::SanitizeCutoff(0.5f), 0.5f);
        EXPECT_FLOAT_EQ(ACM::SanitizeCutoff(1.0f), 1.0f);
        EXPECT_FLOAT_EQ(ACM::SanitizeCutoff(0.0f), 0.0f);
        EXPECT_FLOAT_EQ(ACM::SanitizeCutoff(-0.25f), 0.0f);
        EXPECT_FLOAT_EQ(ACM::SanitizeCutoff(1.5f), 0.0f);
        EXPECT_FLOAT_EQ(ACM::SanitizeCutoff(std::numeric_limits<f32>::quiet_NaN()), 0.0f);
        EXPECT_FLOAT_EQ(ACM::SanitizeCutoff(std::numeric_limits<f32>::infinity()), 0.0f);
    }

    TEST(AlphaCoverageMips, CoverageIsTheShadersComparisonPerByte)
    {
        // 127/255 = 0.498 is discarded at 0.5, 128/255 = 0.502 survives, and a
        // texel exactly at the cutoff survives (`alpha < cutoff` is false).
        const std::vector<u8> rgba = { 0, 0, 0, 127, 0, 0, 0, 128, 0, 0, 0, 255, 0, 0, 0, 0 };
        EXPECT_FLOAT_EQ(ACM::Coverage(rgba, 0.5f), 0.5f);
        EXPECT_FLOAT_EQ(ACM::Coverage(rgba, 127.0f / 255.0f), 0.75f);
        EXPECT_FLOAT_EQ(ACM::Coverage({}, 0.5f), 0.0f);
    }

    TEST(AlphaCoverageMips, APlainBoxChainThinsASparseCutoutAndTheMatchedChainHoldsEveryCutoff)
    {
        constexpr u32 kSize = 512;
        constexpr u32 kLevels = 10; // 512 .. 1
        const std::vector<u8> level0 = SparseCutout(kSize, kSize);
        const f32 base = ACM::Coverage(level0, 0.5f);
        ASSERT_GT(base, 0.25f);
        ASSERT_LT(base, 0.35f);

        const ACM::Chain plain = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/false, /*preserveCoverage=*/false);
        const ACM::Chain matched = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/false, /*preserveCoverage=*/true);
        ASSERT_EQ(plain.Levels.Num(), static_cast<i32>(kLevels - 1u));
        ASSERT_EQ(matched.Levels.Num(), static_cast<i32>(kLevels - 1u));

        // The control: by level 2 (128x128) a box-filtered 30% cutout has almost
        // nothing left above 0.5. If this ever stops being true the fixture no
        // longer demonstrates the defect and the assertions below prove nothing.
        EXPECT_LT(ACM::Coverage(LevelBytes(plain, 1), 0.5f), 0.5f * base)
            << "the plain chain no longer thins this cutout; the control is void";

        // Every level an alpha-tested texture keeps (down to 64x64), at every
        // cutoff the repository uses: level 0's coverage to within one texel of
        // the level (0.024% at 64x64). A binary cutout covers the same fraction
        // at every cutoff, so one chain is right for all of them.
        const u32 kept = ACM::CappedLevelCount(kSize, kSize, kLevels);
        ASSERT_EQ(kept, 4u) << "512 -> 256 -> 128 -> 64";
        for (const f32 cutoff : { 0.25f, 0.3f, 0.5f, 0.75f })
        {
            for (i32 i = 0; i + 1 < static_cast<i32>(kept); ++i)
            {
                const ACM::Level& level = matched.Levels[i];
                EXPECT_NEAR(ACM::Coverage(LevelBytes(matched, i), cutoff), base, OneTexel(level))
                    << "cutoff " << cutoff << ", level " << (i + 1) << " (" << level.Width << "x" << level.Height << ")";
            }
        }
    }

    TEST(AlphaCoverageMips, ASoftCutoutKeepsItsCoverageAtEveryCutoffAtOnce)
    {
        // Soft edges: every opaque texel of the sparse cutout bleeds a ramp
        // into a transparent right-hand neighbour, so level 0 passes a
        // DIFFERENT fraction at each cutoff. A per-cutoff scale (#1441's
        // Castaño chain) could match one of these fractions per chain; the
        // matched chain holds all four.
        constexpr u32 kSize = 256;
        constexpr u32 kLevels = 9;
        std::vector<u8> level0 = SparseCutout(kSize, kSize);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x + 1 < kSize; ++x)
            {
                const u8* texel = &level0[(static_cast<sizet>(y) * kSize + x) * 4u];
                u8* right = &level0[(static_cast<sizet>(y) * kSize + x + 1u) * 4u];
                if (texel[3] == 255u && right[3] == 0u)
                    right[3] = static_cast<u8>(40u + ((x * 37u + y * 11u) % 180u));
            }
        }
        ASSERT_GT(ACM::Coverage(level0, 0.25f), ACM::Coverage(level0, 0.75f) + 0.05f)
            << "the fixture's edges are not soft enough for the cutoffs to disagree";

        const ACM::Chain matched = ACM::Build(level0, kSize, kSize, kLevels, false, true);
        const u32 kept = ACM::CappedLevelCount(kSize, kSize, kLevels);
        ASSERT_EQ(kept, 3u) << "256 -> 128 -> 64";
        for (const f32 cutoff : { 0.25f, 0.3f, 0.5f, 0.75f })
        {
            const f32 base = ACM::Coverage(level0, cutoff);
            for (i32 i = 0; i + 1 < static_cast<i32>(kept); ++i)
            {
                EXPECT_NEAR(ACM::Coverage(LevelBytes(matched, i), cutoff), base, OneTexel(matched.Levels[i]))
                    << "cutoff " << cutoff << ", level " << (i + 1);
            }
        }
    }

    TEST(AlphaCoverageMips, TheRemapKeepsABinaryCardBinaryAndAUniformFootprintAsItWas)
    {
        // The remap only redistributes level 0's alphas: a binary card stays
        // binary, and a texel whose whole footprint in level 0 was opaque (or
        // transparent) stays opaque (or transparent). The remap moves coverage
        // only across texels whose footprints were mixed. (It used to be
        // monotone in the footprint's AVERAGE alpha. That is the rule #1491
        // broke: it put coverage in the right amount but the wrong place.)
        constexpr u32 kSize = 256;
        constexpr u32 kLevels = 9;
        std::vector<u8> level0 = SparseCutout(kSize, kSize);
        // A solid block and an empty block, both aligned to level 2's footprint.
        for (u32 y = 0; y < 64u; ++y)
        {
            for (u32 x = 0; x < 64u; ++x)
            {
                level0[(static_cast<sizet>(y) * kSize + x) * 4u + 3u] = 255u;
                level0[(static_cast<sizet>(y + 128u) * kSize + x + 128u) * 4u + 3u] = 0u;
            }
        }
        const ACM::Chain matched = ACM::Build(level0, kSize, kSize, kLevels, false, true);
        const i32 keptBelowBase = static_cast<i32>(ACM::CappedLevelCount(kSize, kSize, kLevels)) - 1;
        ASSERT_EQ(keptBelowBase, 2) << "256 -> 128 -> 64";

        for (i32 i = 0; i < keptBelowBase; ++i)
        {
            const ACM::Level& level = matched.Levels[i];
            const std::span<const u8> m = LevelBytes(matched, i);
            const u32 scale = kSize / level.Width;
            for (u32 y = 0; y < level.Height; ++y)
            {
                for (u32 x = 0; x < level.Width; ++x)
                {
                    const u8 alpha = m[(static_cast<sizet>(y) * level.Width + x) * 4u + 3u];
                    ASSERT_TRUE(alpha == 0u || alpha == 255u)
                        << "level " << (i + 1) << " invented alpha " << static_cast<u32>(alpha);
                    u32 opaque = 0;
                    for (u32 fy = 0; fy < scale; ++fy)
                    {
                        for (u32 fx = 0; fx < scale; ++fx)
                            opaque += level0[(static_cast<sizet>(y * scale + fy) * kSize + x * scale + fx) * 4u + 3u] == 255u ? 1u : 0u;
                    }
                    if (opaque == scale * scale)
                    {
                        ASSERT_EQ(alpha, 255u) << "level " << (i + 1) << " (" << x << ", " << y
                                               << "): a solid footprint lost its coverage";
                    }
                    if (opaque == 0u)
                    {
                        ASSERT_EQ(alpha, 0u) << "level " << (i + 1) << " (" << x << ", " << y
                                             << "): an empty footprint gained coverage";
                    }
                }
            }
        }
    }

    // Issue #1491: each kept level holds its coverage LOCALLY, not only in
    // total. Judged per aligned 8x8 tile of a level against that tile's
    // footprint in level 0, at every cutoff the shaders use. A tile of 64
    // texels can represent coverage in steps of 1/64, so that is the bound.
    // The first four fixtures tie every texel of a level on the footprint
    // average, which is what the old ranking resolved by texel order (half the
    // level got all the coverage: a 0.5 error).
    TEST(AlphaCoverageMips, LocalCoverageHoldsInEveryTileOfEveryKeptLevel)
    {
        constexpr u32 kTile = 8;
        const auto alphaImage = [](u32 size, auto&& alphaAt)
        {
            std::vector<u8> rgba(static_cast<sizet>(size) * size * 4u, 128u);
            for (u32 y = 0; y < size; ++y)
            {
                for (u32 x = 0; x < size; ++x)
                    rgba[(static_cast<sizet>(y) * size + x) * 4u + 3u] = static_cast<u8>(alphaAt(x, y));
            }
            return rgba;
        };
        const auto checker = [](u32 x, u32 y) -> u32 { return ((x + y) & 1u) != 0u ? 255u : 0u; };
        const auto stripes1 = [](u32 x, u32) -> u32 { return (x & 1u) != 0u ? 255u : 0u; };
        const auto stripes2 = [](u32 x, u32) -> u32 { return (x & 2u) != 0u ? 255u : 0u; };
        // An atlas: a 75% island, a 25% island, a solid island, a 50% island and
        // empty space, each on multiples of 64 texels so every aligned tile of
        // every kept level sees one kind of content.
        const auto islands = [](u32 x, u32 y) -> u32
        {
            const u32 bx = x / 64u;
            const u32 by = y / 64u;
            const bool odd = ((x + y) & 1u) != 0u;
            const bool quarter = (x & 1u) != 0u && (y & 1u) != 0u;
            if (by == 0u && bx < 2u)
                return !quarter ? 255u : 0u; // 75% island
            if (by == 1u && bx >= 2u)
                return quarter ? 255u : 0u; // 25% island
            if (by == 2u && bx == 1u)
                return 255u; // solid island
            if (by == 3u && bx == 3u)
                return odd ? 255u : 0u; // 50% island
            return 0u;
        };
        // A soft 0.5 plateau beside a 50% checker: ranking by the footprint MEAN
        // tied them, and at a 0.75 cutoff the plateau took every 255.
        const auto plateau = [](u32 x, u32 y) -> u32
        { return x < 128u ? 128u : (((x + y) & 1u) != 0u ? 255u : 0u); };

        struct Fixture
        {
            const char* Name;
            u32 Size;
            std::vector<u8> Rgba;
        };
        const std::array<Fixture, 7> fixtures = { {
            { "checker 128", 128, alphaImage(128, checker) },
            { "checker 256", 256, alphaImage(256, checker) },
            { "1-texel stripes", 256, alphaImage(256, stripes1) },
            { "2-texel stripes", 256, alphaImage(256, stripes2) },
            { "atlas islands", 256, alphaImage(256, islands) },
            { "soft plateau beside a checker", 256, alphaImage(256, plateau) },
            { "checker 512", 512, alphaImage(512, checker) },
        } };

        for (const Fixture& fixture : fixtures)
        {
            SCOPED_TRACE(fixture.Name);
            const auto levels = static_cast<u32>(std::bit_width(fixture.Size));
            const ACM::Chain matched = ACM::Build(fixture.Rgba, fixture.Size, fixture.Size, levels, false, true);
            const i32 kept = static_cast<i32>(ACM::CappedLevelCount(fixture.Size, fixture.Size, levels)) - 1;
            ASSERT_GE(kept, 1);
            for (i32 i = 0; i < kept; ++i)
            {
                const ACM::Level& level = matched.Levels[i];
                const std::span<const u8> m = LevelBytes(matched, i);
                const u32 scale = fixture.Size / level.Width;
                const auto passes = [](u8 alpha, f32 cutoff) -> u32
                { return !(static_cast<f32>(alpha) / 255.0f < cutoff) ? 1u : 0u; };
                for (const f32 cutoff : { 0.25f, 0.3f, 0.5f, 0.75f })
                {
                    f32 worst = 0.0f;
                    for (u32 ty = 0; ty + kTile <= level.Height; ty += kTile)
                    {
                        for (u32 tx = 0; tx + kTile <= level.Width; tx += kTile)
                        {
                            u32 levelPass = 0;
                            for (u32 y = ty; y < ty + kTile; ++y)
                            {
                                for (u32 x = tx; x < tx + kTile; ++x)
                                    levelPass += passes(m[(static_cast<sizet>(y) * level.Width + x) * 4u + 3u], cutoff);
                            }
                            u32 sourcePass = 0;
                            for (u32 y = ty * scale; y < (ty + kTile) * scale; ++y)
                            {
                                for (u32 x = tx * scale; x < (tx + kTile) * scale; ++x)
                                    sourcePass += passes(fixture.Rgba[(static_cast<sizet>(y) * fixture.Size + x) * 4u + 3u], cutoff);
                            }
                            const f32 levelCoverage = static_cast<f32>(levelPass) / static_cast<f32>(kTile * kTile);
                            const f32 sourceCoverage =
                                static_cast<f32>(sourcePass) / static_cast<f32>(kTile * kTile * scale * scale);
                            worst = std::max(worst, std::abs(levelCoverage - sourceCoverage));
                        }
                    }
                    EXPECT_LE(worst, 1.0f / 64.0f + 1e-6f)
                        << "level " << (i + 1) << " at cutoff " << cutoff << ": an 8x8 tile's coverage is " << worst
                        << " away from its footprint's in level 0";
                }
                // The global contract still holds.
                EXPECT_LE(ACM::WorstCoverageDifference(m, fixture.Rgba), OneTexel(level) + 1e-6f);
            }
        }
    }

    TEST(AlphaCoverageMips, TheMatchedChainIsDeterministic)
    {
        constexpr u32 kSize = 128;
        const std::vector<u8> level0 = SparseCutout(kSize, kSize);
        const ACM::Chain a = ACM::Build(level0, kSize, kSize, 8, true, true);
        const ACM::Chain b = ACM::Build(level0, kSize, kSize, 8, true, true);
        ASSERT_EQ(a.Bytes.Num(), b.Bytes.Num());
        for (i64 i = 0; i < a.Bytes.Num(); ++i)
            ASSERT_EQ(a.Bytes[i], b.Bytes[i]) << "byte " << i;
    }

    TEST(AlphaCoverageMips, TheChainStopsWhereALevelCanNoLongerHoldCoverage)
    {
        // Through the last level whose SHORTER side is >= 64 texels.
        EXPECT_EQ(ACM::CappedLevelCount(512, 512, 10), 4u);  // 512 .. 64
        EXPECT_EQ(ACM::CappedLevelCount(2048, 256, 12), 3u); // 256 .. 64 on the short side
        EXPECT_EQ(ACM::CappedLevelCount(256, 64, 9), 1u);    // already at the floor
        EXPECT_EQ(ACM::CappedLevelCount(16, 16, 5), 1u);     // below it: never fewer than one
        EXPECT_EQ(ACM::CappedLevelCount(512, 512, 2), 2u);   // never more than asked for
        EXPECT_EQ(ACM::kMinCoarsestExtent, 64u);
    }

    TEST(AlphaCoverageMips, OnlyAlphaDiffersFromThePlainChain)
    {
        constexpr u32 kSize = 64;
        constexpr u32 kLevels = 7;
        const std::vector<u8> level0 = SparseCutout(kSize, kSize);
        const ACM::Chain plain = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/true, false);
        const ACM::Chain matched = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/true, true);
        ASSERT_EQ(plain.Bytes.Num(), matched.Bytes.Num());

        i64 alphaChanged = 0;
        for (i64 i = 0; i < plain.Bytes.Num(); ++i)
        {
            if ((i % 4) == 3)
                alphaChanged += plain.Bytes[i] != matched.Bytes[i] ? 1 : 0;
            else
                ASSERT_EQ(plain.Bytes[i], matched.Bytes[i]) << "colour byte " << i << " moved";
        }
        EXPECT_GT(alphaChanged, 0) << "the matched chain changed nothing on a cutout that thins";
    }

    TEST(AlphaCoverageMips, AnOpaqueTextureMatchesToThePlainChain)
    {
        constexpr u32 kSize = 32;
        constexpr u32 kLevels = 6;
        std::vector<u8> opaque = SparseCutout(kSize, kSize);
        for (sizet i = 3; i < opaque.size(); i += 4)
            opaque[i] = 255u;

        const ACM::Chain plain = ACM::Build(opaque, kSize, kSize, kLevels, false, false);
        const ACM::Chain matched = ACM::Build(opaque, kSize, kSize, kLevels, false, true);
        ASSERT_EQ(plain.Bytes.Num(), matched.Bytes.Num());
        for (i64 i = 0; i < plain.Bytes.Num(); ++i)
            ASSERT_EQ(plain.Bytes[i], matched.Bytes[i]) << "byte " << i;
        for (i64 i = 3; i < plain.Bytes.Num(); i += 4)
            ASSERT_EQ(plain.Bytes[i], 255u) << "an opaque texel lost alpha at byte " << i;
    }

    TEST(AlphaCoverageMips, OnlyAFiniteCutoffWithTexelsOnBothSidesHasCoverageToKeep)
    {
        const std::vector<u8> cutout = SparseCutout(32, 32);
        EXPECT_TRUE(ACM::HasPartialCoverage(cutout, 0.5f));
        EXPECT_FALSE(ACM::HasPartialCoverage(cutout, 0.0f)) << "0 means not alpha-tested";
        EXPECT_FALSE(ACM::HasPartialCoverage(cutout, std::numeric_limits<f32>::quiet_NaN()));
        std::vector<u8> opaque = cutout;
        for (sizet i = 3; i < opaque.size(); i += 4)
            opaque[i] = 255u;
        EXPECT_FALSE(ACM::HasPartialCoverage(opaque, 0.5f)) << "passes everywhere: nothing to keep";
    }

    TEST(AlphaCoverageMips, TheCookRecognisesACutoutByItsAlphaAlone)
    {
        // The gate the cook uses in place of a consumer's cutoff. The
        // thresholds come from the repository's own textures (the header);
        // these are the cases on each side of them.
        const std::vector<u8> binary = SparseCutout(64, 64);
        EXPECT_TRUE(ACM::IsCutoutAlpha(binary));

        std::vector<u8> opaque = binary;
        for (sizet i = 3; i < opaque.size(); i += 4)
            opaque[i] = 255u;
        EXPECT_FALSE(ACM::IsCutoutAlpha(opaque)) << "no transparent texel: not a cutout";

        std::vector<u8> invisible = binary;
        for (sizet i = 3; i < invisible.size(); i += 4)
            invisible[i] = 0u;
        EXPECT_FALSE(ACM::IsCutoutAlpha(invisible)) << "no opaque texel: not a cutout";

        // Data carried in alpha (a height ramp): nearly all of it in between.
        std::vector<u8> ramp = binary;
        for (sizet i = 3, t = 0; i < ramp.size(); i += 4, ++t)
            ramp[i] = static_cast<u8>(t % 256u);
        EXPECT_FALSE(ACM::IsCutoutAlpha(ramp));

        // The boundary: a third in between is still a cutout, more is not.
        // 12 texels: 4 transparent, 4 opaque, 4 at 128 -> exactly a third.
        std::vector<u8> third(12u * 4u, 0u);
        for (u32 t = 0; t < 12u; ++t)
            third[t * 4u + 3u] = t < 4u ? 0u : (t < 8u ? 255u : 128u);
        EXPECT_TRUE(ACM::IsCutoutAlpha(third));
        third[4u * 4u + 3u] = 128u; // one opaque texel moves into the middle
        EXPECT_FALSE(ACM::IsCutoutAlpha(third));
    }

    TEST(AlphaCoverageMips, LevelExtentsFollowTheGraphicsApis)
    {
        const std::vector<u8> level0(64u * 16u * 4u, 255u);
        const ACM::Chain chain = ACM::Build(level0, 64, 16, 7, false, false);
        const std::vector<std::pair<u32, u32>> expected = { { 32, 8 }, { 16, 4 }, { 8, 2 }, { 4, 1 }, { 2, 1 }, { 1, 1 } };
        ASSERT_EQ(chain.Levels.Num(), static_cast<i32>(expected.size()));
        u64 offset = 0;
        for (i32 i = 0; i < chain.Levels.Num(); ++i)
        {
            EXPECT_EQ(chain.Levels[i].Width, expected[static_cast<sizet>(i)].first) << "level " << (i + 1);
            EXPECT_EQ(chain.Levels[i].Height, expected[static_cast<sizet>(i)].second) << "level " << (i + 1);
            EXPECT_EQ(chain.Levels[i].Offset, offset);
            offset += static_cast<u64>(chain.Levels[i].Width) * chain.Levels[i].Height * 4u;
        }
        EXPECT_EQ(static_cast<u64>(chain.Bytes.Num()), offset);
    }

    TEST(AlphaCoverageMips, ColourIsAveragedInLinearLightForAnSrgbTexture)
    {
        // Black beside white. Linear data averages to 128; sRGB data averages
        // the LIGHT, 0.5 linear, which encodes to 188.
        const std::vector<u8> level0 = { 0, 0, 0, 255, 255, 255, 255, 255 };
        const ACM::Chain linear = ACM::Build(level0, 2, 1, 2, /*srgb=*/false, false);
        const ACM::Chain srgb = ACM::Build(level0, 2, 1, 2, /*srgb=*/true, false);
        EXPECT_EQ(linear.Bytes[0], 128u);
        EXPECT_EQ(srgb.Bytes[0], 188u);
        EXPECT_EQ(srgb.Bytes[3], 255u) << "alpha is never sRGB-decoded";
    }

    TEST(AlphaCoverageMips, AnOddExtentDropsNoTexel)
    {
        // 3 -> 1: all three texels weigh a third. A 2-tap filter would read
        // 255 or 128 depending on which pair it took; the area weight reads 170.
        const std::vector<u8> level0 = { 255, 0, 0, 255, 0, 0, 0, 255, 255, 0, 0, 255 };
        const ACM::Chain chain = ACM::Build(level0, 3, 1, 2, false, false);
        ASSERT_EQ(chain.Levels.Num(), 1);
        EXPECT_EQ(chain.Levels[0].Width, 1u);
        EXPECT_EQ(chain.Bytes[0], 170u);
    }
} // namespace OloEngine::Tests
