// =============================================================================
// AlphaCoverageMipsContractTest.cpp
//
// The CPU half of issue #1441: a mip chain for an alpha-TESTED texture keeps the
// fraction of texels that pass the alpha test the same at every level. Pinned on
// synthetic images, so every expected number is exact:
//
//   1. The coverage count is the shaders' `alpha < cutoff` comparison per UNORM
//      byte, and only a finite cutoff in (0, 1] drives preservation.
//   2. A plain box chain THINS a sparse cutout (the control), and the preserved
//      chain holds level 0's coverage at every level with texels to spare.
//   3. Only alpha differs from the plain chain: colour is bit-identical, an
//      opaque texture is untouched, and a zero cutoff IS the plain chain.
//   4. The box filter itself: extents follow max(1, n / 2), colour is averaged in
//      linear light for an sRGB texture, and an odd extent drops no texel.
//
// The GPU half — the chain reaching a real GL texture through
// Texture2D::SetAlphaCoverageCutoff, and surviving a reload — is in
// TextureInPlaceReloadTest.
//
// OLO_TEST_LAYER: L1
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <span>
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
    } // namespace

    TEST(AlphaCoverageMips, OnlyAFiniteCutoffInsideTheUnitIntervalDrivesPreservation)
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

    TEST(AlphaCoverageMips, APlainBoxChainThinsASparseCutoutAndThePreservedChainDoesNot)
    {
        constexpr u32 kSize = 512;
        constexpr u32 kLevels = 10; // 512 .. 1
        constexpr f32 kCutoff = 0.5f;
        const std::vector<u8> level0 = SparseCutout(kSize, kSize);
        const f32 base = ACM::Coverage(level0, kCutoff);
        ASSERT_GT(base, 0.25f);
        ASSERT_LT(base, 0.35f);

        const ACM::Chain plain = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/false, /*cutoff=*/0.0f);
        const ACM::Chain preserved = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/false, kCutoff);
        ASSERT_EQ(plain.Levels.Num(), static_cast<i32>(kLevels - 1u));
        ASSERT_EQ(preserved.Levels.Num(), static_cast<i32>(kLevels - 1u));

        // The control: by level 2 (128x128) a box-filtered 30% cutout has almost
        // nothing left above 0.5. If this ever stops being true the fixture no
        // longer demonstrates the defect and the assertions below prove nothing.
        EXPECT_LT(ACM::Coverage(LevelBytes(plain, 1), kCutoff), 0.5f * base)
            << "the plain chain no longer thins this cutout; the control is void";

        // Only the levels an alpha-tested texture keeps (down to 64x64). A
        // single scale can only land on the coverages the level's alpha
        // histogram offers: a binary cutout's level 1 holds 5 alpha values, so
        // the nearest reachable coverage here is 34% for a 30% target. 5
        // points is that granularity; the plain chain is off by 20 or more.
        const u32 kept = ACM::CappedLevelCount(kSize, kSize, kLevels);
        ASSERT_EQ(kept, 4u) << "512 -> 256 -> 128 -> 64";
        for (i32 i = 0; i + 1 < static_cast<i32>(kept); ++i)
        {
            const ACM::Level& level = preserved.Levels[i];
            EXPECT_NEAR(ACM::Coverage(LevelBytes(preserved, i), kCutoff), base, 0.05f)
                << "level " << (i + 1) << " (" << level.Width << "x" << level.Height << ")";
        }
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
        const ACM::Chain plain = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/true, 0.0f);
        const ACM::Chain preserved = ACM::Build(level0, kSize, kSize, kLevels, /*srgb=*/true, 0.5f);
        ASSERT_EQ(plain.Bytes.Num(), preserved.Bytes.Num());

        i64 alphaChanged = 0;
        for (i64 i = 0; i < plain.Bytes.Num(); ++i)
        {
            if ((i % 4) == 3)
                alphaChanged += plain.Bytes[i] != preserved.Bytes[i] ? 1 : 0;
            else
                ASSERT_EQ(plain.Bytes[i], preserved.Bytes[i]) << "colour byte " << i << " moved";
        }
        EXPECT_GT(alphaChanged, 0) << "the preserved chain changed nothing on a cutout that thins";
    }

    TEST(AlphaCoverageMips, AnOpaqueTextureAndAZeroCutoffAreThePlainChain)
    {
        constexpr u32 kSize = 32;
        constexpr u32 kLevels = 6;
        std::vector<u8> opaque = SparseCutout(kSize, kSize);
        for (sizet i = 3; i < opaque.size(); i += 4)
            opaque[i] = 255u;

        const ACM::Chain plain = ACM::Build(opaque, kSize, kSize, kLevels, false, 0.0f);
        const ACM::Chain preserved = ACM::Build(opaque, kSize, kSize, kLevels, false, 0.5f);
        ASSERT_EQ(plain.Bytes.Num(), preserved.Bytes.Num());
        for (i64 i = 0; i < plain.Bytes.Num(); ++i)
            ASSERT_EQ(plain.Bytes[i], preserved.Bytes[i]) << "byte " << i;
        for (i64 i = 3; i < plain.Bytes.Num(); i += 4)
            ASSERT_EQ(plain.Bytes[i], 255u) << "an opaque texel lost alpha at byte " << i;

        // NaN and out-of-range cutoffs sanitise to "plain", never to a guess.
        const std::vector<u8> cutout = SparseCutout(kSize, kSize);
        const ACM::Chain reference = ACM::Build(cutout, kSize, kSize, kLevels, false, 0.0f);
        const ACM::Chain nan = ACM::Build(cutout, kSize, kSize, kLevels, false, std::numeric_limits<f32>::quiet_NaN());
        ASSERT_EQ(reference.Bytes.Num(), nan.Bytes.Num());
        for (i64 i = 0; i < reference.Bytes.Num(); ++i)
            ASSERT_EQ(reference.Bytes[i], nan.Bytes[i]) << "byte " << i;
    }

    TEST(AlphaCoverageMips, LevelExtentsFollowTheGraphicsApis)
    {
        const std::vector<u8> level0(64u * 16u * 4u, 255u);
        const ACM::Chain chain = ACM::Build(level0, 64, 16, 7, false, 0.0f);
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
        const ACM::Chain linear = ACM::Build(level0, 2, 1, 2, /*srgb=*/false, 0.0f);
        const ACM::Chain srgb = ACM::Build(level0, 2, 1, 2, /*srgb=*/true, 0.0f);
        EXPECT_EQ(linear.Bytes[0], 128u);
        EXPECT_EQ(srgb.Bytes[0], 188u);
        EXPECT_EQ(srgb.Bytes[3], 255u) << "alpha is never sRGB-decoded";
    }

    TEST(AlphaCoverageMips, AnOddExtentDropsNoTexel)
    {
        // 3 -> 1: all three texels weigh a third. A 2-tap filter would read
        // 255 or 128 depending on which pair it took; the area weight reads 170.
        const std::vector<u8> level0 = { 255, 0, 0, 255, 0, 0, 0, 255, 255, 0, 0, 255 };
        const ACM::Chain chain = ACM::Build(level0, 3, 1, 2, false, 0.0f);
        ASSERT_EQ(chain.Levels.Num(), 1);
        EXPECT_EQ(chain.Levels[0].Width, 1u);
        EXPECT_EQ(chain.Bytes[0], 170u);
    }
} // namespace OloEngine::Tests
