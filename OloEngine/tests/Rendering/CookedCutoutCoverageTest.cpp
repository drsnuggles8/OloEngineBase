// =============================================================================
// CookedCutoutCoverageTest.cpp — issue #1453, the COOKED half of #1441.
//
// A cooked BC7 cutout (the format the asset pack ships) reaches the GPU with the
// mip chain the cook baked; nothing at runtime can rebuild it. So the cook itself
// has to produce a chain that keeps the texture's alpha-test coverage at every
// level, and it has to do so without knowing the cutoff: the same card is drawn
// at 0.25, 0.3 and 0.5 by different consumers (pine_card, shrub_card,
// broadleaf_card), and AlphaBlendModeTest.gltf uses 0.75.
//
// Pinned here, on the repository's real textures, through the real cook
// (TextureCompression::CompressImageFile) and the real BC7 DECODE — the levels a
// GPU would sample, after the block encoder has perturbed their alpha:
//
//   1. every kept level's coverage matches the cooked level 0's to within half a
//      coverage point at 0.25, 0.3, 0.5 AND 0.75, for binary cards and for the
//      soft-alpha grass.png and Sponza Mask PNG;
//   2. the chain stops at AlphaCoverageMips::CappedLevelCount levels;
//   3. the control: the same texture cooked with a plain box chain loses most of
//      its coverage by the coarsest kept level, so (1) is not vacuous;
//   4. only alpha differs from the plain cook, and a texture whose alpha is not
//      a cutout's keeps the full averaged chain;
//   5. each aligned 8x8 tile of a decoded level keeps its footprint's coverage
//      (#1491), not only the level as a whole.
//
// Pure CPU; runs in CI. The on-screen half is CookedCutoutCoverageEvidenceTest.
//
// OLO_TEST_LAYER: L1
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#define OLO_TEST_EDITOR_ROOT ""
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace ACM = AlphaCoverageMips;

        constexpr std::array<f32, 4> kCutoffs = { 0.25f, 0.3f, 0.5f, 0.75f };

        // Half a coverage point. Before encode the matched chain is exact to one
        // texel (AlphaCoverageMipsContractTest); BC7 then moves alpha by a few
        // codes per block, and the cook re-encodes each level against what it
        // decodes to (TextureCompression EncodeCoverageLevel). Measured on these
        // textures with that correction (#1453): 0.3 points at worst, on the
        // soft Sponza texture, which was 1.4 without it.
        constexpr f32 kCookedTolerance = 0.005f;

        struct Subject
        {
            const char* Path;
            const char* Kind;
        };

        // Binary cards from the vegetation set (#1398) and the soft-alpha
        // cutouts: the foliage fixtures' grass.png and a Sponza Mask texture.
        constexpr std::array<Subject, 5> kSubjects = { {
            { "SandboxProject/Assets/Models/Vegetation/pine/Textures/pine_card.png", "binary" },
            { "SandboxProject/Assets/Models/Vegetation/grass/Textures/grass_card.png", "binary" },
            { "SandboxProject/Assets/Models/Vegetation/broadleaf/Textures/broadleaf_card.png", "binary" },
            { "assets/textures/grass.png", "soft" },
            { "SandboxProject/Assets/Models/Sponza/5061699253647017043.png", "soft" },
        } };

        [[nodiscard]] std::string EditorPath(const char* relative)
        {
            return (std::filesystem::path(OLO_TEST_EDITOR_ROOT) / relative).string();
        }

        [[nodiscard]] TextureCompression::CompressOptions AutoOptions()
        {
            TextureCompression::CompressOptions options;
            // Exactly what the pack cook passes, minus any sidecar in the tree.
            options.UseImportSettings = false;
            return options;
        }

        [[nodiscard]] f32 DecodedCoverage(const CompressedTextureImage& image, u32 level, f32 cutoff)
        {
            TArray64<u8> rgba;
            u32 w = 0;
            u32 h = 0;
            if (!TextureCompression::DecodeToRGBA8(image, level, rgba, w, h))
            {
                ADD_FAILURE() << "decode of level " << level << " failed";
                return -1.0f;
            }
            return ACM::Coverage({ rgba.GetData(), static_cast<sizet>(rgba.Num()) }, cutoff);
        }

        // The same texture cooked with the plain box chain: the control arm.
        [[nodiscard]] CompressedTextureImage CookPlainChain(const std::string& path, bool srgb)
        {
            ::stbi_set_flip_vertically_on_load_thread(1);
            int w = 0;
            int h = 0;
            int channels = 0;
            stbi_uc* pixels = ::stbi_load(path.c_str(), &w, &h, &channels, 4);
            ::stbi_set_flip_vertically_on_load_thread(0);
            CompressedTextureImage image;
            if (pixels == nullptr)
                return image;
            image = TextureCompression::EncodeBC7(pixels, static_cast<u32>(w), static_cast<u32>(h), 4, srgb,
                                                  /*generateMips=*/true, /*preserveAlphaCoverage=*/false);
            ::stbi_image_free(pixels);
            return image;
        }
    } // namespace

    TEST(CookedCutoutCoverage, EveryKeptLevelKeepsLevelZerosCoverageAtEveryCutoff)
    {
        for (const Subject& subject : kSubjects)
        {
            const std::string path = EditorPath(subject.Path);
            SCOPED_TRACE(path);
            ASSERT_TRUE(std::filesystem::exists(path)) << "fixture texture missing";

            CompressedTextureImage cooked;
            ASSERT_TRUE(TextureCompression::CompressImageFile(path, AutoOptions(), cooked));
            ASSERT_EQ(cooked.Format, TextureCompressionFormat::BC7);
            ASSERT_TRUE(cooked.HasAlpha);
            EXPECT_EQ(cooked.MipLevels(), ACM::CappedLevelCount(cooked.Width, cooked.Height, 32u))
                << "a cutout's cooked chain stops at the last level that can hold its coverage";
            ASSERT_GT(cooked.MipLevels(), 2u) << "these textures are large enough to keep several levels";

            const CompressedTextureImage plain = CookPlainChain(path, cooked.SRGB);
            ASSERT_TRUE(plain.IsValid());
            const u32 coarsest = cooked.MipLevels() - 1u;
            f32 plainWorst = 0.0f;

            for (const f32 cutoff : kCutoffs)
            {
                SCOPED_TRACE(testing::Message() << subject.Kind << " cutoff " << cutoff);
                const f32 base = DecodedCoverage(cooked, 0, cutoff);
                ASSERT_GT(base, 0.02f) << "the card covers almost nothing at this cutoff";
                ASSERT_LT(base, 0.98f);

                for (u32 level = 1; level < cooked.MipLevels(); ++level)
                {
                    EXPECT_NEAR(DecodedCoverage(cooked, level, cutoff), base, kCookedTolerance)
                        << "cooked level " << level << " of " << cooked.MipLevels();
                }

                plainWorst = std::max(plainWorst, std::abs(DecodedCoverage(plain, coarsest, cutoff) - base));
            }

            // The control has to miss where the matched chain holds, or holding
            // proves nothing. At 0.5 a box filter keeps these shapes' coverage
            // fairly well down to 64 texels; at the other cutoffs it thickens
            // (below 0.5) or thins (above). Measured: 2 to 9 points at the
            // coarsest kept level, at the worst of the four cutoffs.
            EXPECT_GT(plainWorst, 4.0f * kCookedTolerance)
                << "the plain box chain kept this texture's coverage at every cutoff — the comparison is vacuous for it";
        }
    }

    TEST(CookedCutoutCoverage, OnlyAlphaDiffersFromThePlainCook)
    {
        // The chain is the plain one with its alpha re-ranked. Level 0 is the
        // source in both cooks, so its blocks are byte-identical; below it the
        // colour was filtered identically, so each level's mean colour agrees
        // to within the block encoder's own noise.
        const std::string path = EditorPath(kSubjects[0].Path);
        CompressedTextureImage cooked;
        ASSERT_TRUE(TextureCompression::CompressImageFile(path, AutoOptions(), cooked));
        const CompressedTextureImage plain = CookPlainChain(path, cooked.SRGB);
        ASSERT_TRUE(plain.IsValid());
        ASSERT_EQ(cooked.Mips[0].Num(), plain.Mips[0].Num());
        EXPECT_TRUE(std::equal(cooked.Mips[0].begin(), cooked.Mips[0].end(), plain.Mips[0].begin()))
            << "level 0 is the source; the two cooks must agree on it";

        for (u32 level = 1; level < cooked.MipLevels(); ++level)
        {
            TArray64<u8> a;
            TArray64<u8> b;
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(TextureCompression::DecodeToRGBA8(cooked, level, a, w, h));
            ASSERT_TRUE(TextureCompression::DecodeToRGBA8(plain, level, b, w, h));
            ASSERT_EQ(a.Num(), b.Num());
            for (u32 c = 0; c < 3u; ++c)
            {
                f64 meanA = 0.0;
                f64 meanB = 0.0;
                for (i64 i = c; i < a.Num(); i += 4)
                {
                    meanA += a[i];
                    meanB += b[i];
                }
                const f64 texels = static_cast<f64>(a.Num() / 4);
                EXPECT_NEAR(meanA / texels, meanB / texels, 2.0) << "level " << level << " channel " << c;
            }
        }
    }

    TEST(CookedCutoutCoverage, ATextureWhoseAlphaIsNotACutoutsKeepsTheFullAveragedChain)
    {
        // Data carried in alpha — a height ramp, as pbr/wall/normal.png has —
        // is all in between. Re-ranking it would scramble the data and capping
        // the chain would alias it, so the cook leaves it alone.
        constexpr u32 kSize = 128;
        std::vector<u8> pixels(static_cast<sizet>(kSize) * kSize * 4u);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                u8* texel = &pixels[(static_cast<sizet>(y) * kSize + x) * 4u];
                texel[0] = 128u;
                texel[1] = 128u;
                texel[2] = 255u;
                texel[3] = static_cast<u8>((x + y) % 256u);
            }
        }
        ASSERT_FALSE(ACM::IsCutoutAlpha({ pixels.data(), pixels.size() }));
        const std::filesystem::path png = TempFile("cooked_cutout_height_alpha.png");
        ASSERT_NE(::stbi_write_png(png.string().c_str(), static_cast<int>(kSize), static_cast<int>(kSize), 4,
                                   pixels.data(), static_cast<int>(kSize) * 4),
                  0);

        CompressedTextureImage cooked;
        ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), AutoOptions(), cooked));
        ASSERT_TRUE(cooked.HasAlpha);
        EXPECT_EQ(cooked.MipLevels(), 8u) << "128 -> 1: the full chain";
    }

    // Issue #1491, cooked: the per-tile placement the loose chain keeps
    // (AlphaCoverageMipsContractTest.LocalCoverageHoldsInEveryTileOfEveryKeptLevel)
    // survives the BC7 encode. A 256 atlas of a 75%, a 25%, a solid and a 50%
    // island, each on 64-texel boundaries: every aligned 8x8 tile of every kept
    // cooked level, decoded, holds its footprint's coverage to within two texels
    // of the tile (one for the matched chain, one for the block encoder). The old
    // global ranking gave the 75% island all of the coverage and the 25% island
    // none, a 0.25 error.
    TEST(CookedCutoutCoverage, TheCookKeepsEachTilesCoverageWhereLevelZeroHadIt)
    {
        constexpr u32 kSize = 256;
        constexpr u32 kTile = 8;
        std::vector<u8> rgba(static_cast<sizet>(kSize) * kSize * 4u);
        for (u32 y = 0; y < kSize; ++y)
        {
            for (u32 x = 0; x < kSize; ++x)
            {
                const u32 bx = x / 64u;
                const u32 by = y / 64u;
                const bool odd = ((x + y) & 1u) != 0u;
                const bool quarter = (x & 1u) != 0u && (y & 1u) != 0u;
                u32 alpha = 0u;
                if (by == 0u && bx < 2u)
                    alpha = !quarter ? 255u : 0u;
                else if (by == 1u && bx >= 2u)
                    alpha = quarter ? 255u : 0u;
                else if (by == 2u && bx == 1u)
                    alpha = 255u;
                else if (by == 3u && bx == 3u)
                    alpha = odd ? 255u : 0u;
                u8* texel = &rgba[(static_cast<sizet>(y) * kSize + x) * 4u];
                texel[0] = static_cast<u8>(60u + x % 97u);
                texel[1] = static_cast<u8>(120u + y % 61u);
                texel[2] = 40u;
                texel[3] = static_cast<u8>(alpha);
            }
        }

        const CompressedTextureImage cooked = TextureCompression::EncodeBC7(
            rgba.data(), kSize, kSize, 4, /*srgb=*/false, /*generateMips=*/true, /*preserveAlphaCoverage=*/true);
        ASSERT_TRUE(cooked.IsValid());
        ASSERT_EQ(cooked.MipLevels(), ACM::CappedLevelCount(kSize, kSize, 9u));

        for (u32 level = 1; level < cooked.MipLevels(); ++level)
        {
            TArray64<u8> decoded;
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(TextureCompression::DecodeToRGBA8(cooked, level, decoded, w, h));
            const u32 scale = kSize / w;
            for (const f32 cutoff : kCutoffs)
            {
                const auto passes = [cutoff](u8 alpha) -> u32
                { return !(static_cast<f32>(alpha) / 255.0f < cutoff) ? 1u : 0u; };
                f32 worst = 0.0f;
                for (u32 ty = 0; ty + kTile <= h; ty += kTile)
                {
                    for (u32 tx = 0; tx + kTile <= w; tx += kTile)
                    {
                        u32 cookedPass = 0;
                        for (u32 y = ty; y < ty + kTile; ++y)
                        {
                            for (u32 x = tx; x < tx + kTile; ++x)
                                cookedPass += passes(decoded[(static_cast<i64>(y) * w + x) * 4 + 3]);
                        }
                        u32 sourcePass = 0;
                        for (u32 y = ty * scale; y < (ty + kTile) * scale; ++y)
                        {
                            for (u32 x = tx * scale; x < (tx + kTile) * scale; ++x)
                                sourcePass += passes(rgba[(static_cast<sizet>(y) * kSize + x) * 4u + 3u]);
                        }
                        worst = std::max(worst, std::abs(static_cast<f32>(cookedPass) / static_cast<f32>(kTile * kTile) -
                                                         static_cast<f32>(sourcePass) /
                                                             static_cast<f32>(kTile * kTile * scale * scale)));
                    }
                }
                EXPECT_LE(worst, 2.0f / 64.0f + 1e-6f)
                    << "cooked level " << level << " at cutoff " << cutoff << ": a decoded 8x8 tile's coverage is " << worst
                    << " away from its footprint's in level 0";
            }
        }
    }
} // namespace OloEngine::Tests
