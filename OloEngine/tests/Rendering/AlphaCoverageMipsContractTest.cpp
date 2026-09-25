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
//   3. The remap only reorders level 0's alphas by footprint: monotone in the
//      box-filtered alpha, a binary card stays binary, and it is deterministic.
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

    TEST(AlphaCoverageMips, TheRemapFollowsTheFootprintAndKeepsABinaryCardBinary)
    {
        // The texels that receive level 0's high alphas are the ones whose
        // footprints held the most alpha. The plain chain's alpha IS the
        // footprint average (rounded), so where it is strictly higher the
        // matched alpha must not be lower. Equal plain alphas may land either
        // way — rounding merged footprints the remap still tells apart.
        //
        // Checked on the levels a cutout keeps. Below them the plain chain's
        // per-level rounding is coarser than the footprints' differences and
        // can itself reorder them, so it stops being a valid reference.
        constexpr u32 kSize = 256;
        constexpr u32 kLevels = 9;
        const std::vector<u8> level0 = SparseCutout(kSize, kSize);
        const ACM::Chain plain = ACM::Build(level0, kSize, kSize, kLevels, false, false);
        const ACM::Chain matched = ACM::Build(level0, kSize, kSize, kLevels, false, true);
        const i32 keptBelowBase = static_cast<i32>(ACM::CappedLevelCount(kSize, kSize, kLevels)) - 1;
        ASSERT_EQ(keptBelowBase, 2) << "256 -> 128 -> 64";

        for (i32 i = 0; i < keptBelowBase; ++i)
        {
            const std::span<const u8> p = LevelBytes(plain, i);
            const std::span<const u8> m = LevelBytes(matched, i);
            // Per plain alpha: the lowest and highest matched alpha it received.
            std::array<i32, 256> lowest;
            std::array<i32, 256> highest;
            lowest.fill(256);
            highest.fill(-1);
            for (sizet t = 3; t < p.size(); t += 4)
            {
                // A binary card stays binary: no texel of a matched level takes
                // an alpha level 0 never had.
                ASSERT_TRUE(m[t] == 0u || m[t] == 255u) << "level " << (i + 1) << " invented alpha " << static_cast<u32>(m[t]);
                lowest[p[t]] = std::min(lowest[p[t]], static_cast<i32>(m[t]));
                highest[p[t]] = std::max(highest[p[t]], static_cast<i32>(m[t]));
            }
            i32 floorFromAbove = 256; // lowest matched alpha among strictly higher plain alphas
            for (i32 a = 255; a >= 0; --a)
            {
                if (highest[a] < 0)
                    continue;
                EXPECT_LE(highest[a], floorFromAbove)
                    << "level " << (i + 1) << ": a footprint of plain alpha " << a
                    << " outranked one with more alpha";
                floorFromAbove = std::min(floorFromAbove, lowest[a]);
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
