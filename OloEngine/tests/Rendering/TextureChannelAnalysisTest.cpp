// OLO_TEST_LAYER: L3
//
// Measured per-channel format selection and BC4 (#624 item 5).
//
// The issue asks for "per-channel / packed-format heuristics". The word heuristic is the
// trap: a rule that guesses from a filename misfires silently on someone's roughness map,
// which is exactly what kept BC5 out of the auto-cook in #440. So every rule tested here
// is a MEASUREMENT of the decoded pixels, and each one is only allowed to change the
// format when doing so cannot change what a shader reads:
//
//   * greyscale (R == G == B at every texel, opaque) -> BC4, half the bytes of BC7, and
//     sampled (R,R,R,1) on both the GPU (a texture swizzle) and the CPU (DecodeToRGBA8);
//   * a 4-channel source whose alpha is a constant 255 is OPAQUE — reporting it as having
//     alpha put an opaque albedo in the transparent render pass;
//   * anything with real colour, or real alpha, stays BC7.
//
// The case deliberately NOT automated is narrowing RGB to two channels: BC5 decodes blue
// as 0, so it would change the data unless blue happened to be 0 already. That one stays
// an explicit sidecar choice, and the last test pins that it does.

#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include <gtest/gtest.h>
#include "TestTempDir.h"
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace OloEngine;

namespace
{
    std::vector<u8> MakeGreyscaleRGBA(u32 width, u32 height, bool varyAlpha)
    {
        std::vector<u8> pixels(static_cast<sizet>(width) * height * 4);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const auto value = static_cast<u8>((x * 255) / std::max(1u, width - 1));
                u8* p = &pixels[(static_cast<sizet>(y) * width + x) * 4];
                p[0] = value;
                p[1] = value;
                p[2] = value;
                p[3] = varyAlpha ? static_cast<u8>((y * 255) / std::max(1u, height - 1)) : 255;
            }
        }
        return pixels;
    }

    std::vector<u8> MakeColorRGBA(u32 width, u32 height, bool varyAlpha)
    {
        std::vector<u8> pixels(static_cast<sizet>(width) * height * 4);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                u8* p = &pixels[(static_cast<sizet>(y) * width + x) * 4];
                p[0] = static_cast<u8>((x * 255) / std::max(1u, width - 1));
                p[1] = static_cast<u8>((y * 255) / std::max(1u, height - 1));
                p[2] = 64;
                p[3] = varyAlpha ? static_cast<u8>(x * 4) : 255;
            }
        }
        return pixels;
    }

    std::filesystem::path WritePng(const char* name, const std::vector<u8>& rgba, u32 width, u32 height, u32 channels)
    {
        const std::filesystem::path path = OloEngine::Tests::TempFile(name);
        std::vector<u8> packed(static_cast<sizet>(width) * height * channels);
        for (sizet i = 0; i < static_cast<sizet>(width) * height; ++i)
        {
            for (u32 c = 0; c < channels; ++c)
                packed[i * channels + c] = rgba[i * 4 + c];
        }
        EXPECT_NE(::stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height),
                                   static_cast<int>(channels), packed.data(),
                                   static_cast<int>(width * channels)),
                  0);
        return path;
    }

    void Remove(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
} // namespace

TEST(TextureChannelAnalysis, BlockGeometryKnowsBC4IsHalfABlock)
{
    // BC4 is the first format here that is not 16 bytes per block. Everything that walks
    // a mip — the container's size check, the CPU decode, the GL upload — derives the
    // stride from this, so getting it wrong reads every block after the first at an
    // offset and produces garbage that still "loads".
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::BC4), 8u);
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::BC5), 16u);
    // 16x16 -> 4x4 blocks -> 16 blocks * 8 bytes.
    EXPECT_EQ(TextureCompression::MipByteSize(TextureCompressionFormat::BC4, 16, 16), 128u);
    EXPECT_EQ(TextureCompression::MipByteSize(TextureCompressionFormat::BC4, 13, 7), 64u);
}

TEST(TextureChannelAnalysis, ReportsWhatTheImageActuallyUses)
{
    constexpr u32 kW = 16;
    constexpr u32 kH = 16;

    const TextureCompression::ChannelUsage grey =
        TextureCompression::AnalyzeChannels(MakeGreyscaleRGBA(kW, kH, false).data(), kW, kH, 4);
    EXPECT_TRUE(grey.IsGreyscale);
    EXPECT_FALSE(grey.HasAlpha) << "a constant-255 alpha is not alpha";
    EXPECT_EQ(grey.VaryingChannels, 3u) << "R, G and B all vary together; alpha does not";

    const TextureCompression::ChannelUsage greyAlpha =
        TextureCompression::AnalyzeChannels(MakeGreyscaleRGBA(kW, kH, true).data(), kW, kH, 4);
    EXPECT_TRUE(greyAlpha.IsGreyscale);
    EXPECT_TRUE(greyAlpha.HasAlpha);

    const TextureCompression::ChannelUsage color =
        TextureCompression::AnalyzeChannels(MakeColorRGBA(kW, kH, false).data(), kW, kH, 4);
    EXPECT_FALSE(color.IsGreyscale);
    EXPECT_FALSE(color.HasAlpha);

    // A 1-channel source is greyscale by construction: ExpandToRGBA8 replicates R.
    std::vector<u8> single(static_cast<sizet>(kW) * kH);
    for (sizet i = 0; i < single.size(); ++i)
        single[i] = static_cast<u8>(i);
    const TextureCompression::ChannelUsage oneChannel =
        TextureCompression::AnalyzeChannels(single.data(), kW, kH, 1);
    EXPECT_TRUE(oneChannel.IsGreyscale);
    EXPECT_FALSE(oneChannel.HasAlpha);
}

TEST(TextureChannelAnalysis, BC4RoundTripsGreyscaleAndSamplesAsRRR1)
{
    constexpr u32 kW = 32;
    constexpr u32 kH = 32;
    const std::vector<u8> source = MakeGreyscaleRGBA(kW, kH, false);

    const CompressedTextureImage image = TextureCompression::EncodeBC4(source.data(), kW, kH, 4, false);
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC4);
    EXPECT_EQ(image.Mips[0].size(), TextureCompression::MipByteSize(TextureCompressionFormat::BC4, kW, kH));

    std::vector<u8> decoded;
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBA8(image, 0, decoded, dw, dh));
    ASSERT_EQ(decoded.size(), static_cast<sizet>(kW) * kH * 4);

    double mse = 0.0;
    for (sizet i = 0; i < static_cast<sizet>(kW) * kH; ++i)
    {
        // The decode must replicate red, or a greyscale texture goes red-tinted the
        // moment the cook narrows it — the substitution has to be invisible.
        EXPECT_EQ(decoded[i * 4 + 1], decoded[i * 4 + 0]) << "texel " << i << ": green must mirror red";
        EXPECT_EQ(decoded[i * 4 + 2], decoded[i * 4 + 0]) << "texel " << i << ": blue must mirror red";
        EXPECT_EQ(decoded[i * 4 + 3], 255);
        const double d = static_cast<double>(source[i * 4]) - static_cast<double>(decoded[i * 4]);
        mse += d * d;
    }
    mse /= static_cast<double>(kW) * kH;
    const double psnr = mse < 1e-9 ? 99.0 : 10.0 * std::log10((255.0 * 255.0) / mse);
    EXPECT_GT(psnr, 40.0) << "BC4 greyscale round-trip PSNR too low: " << psnr << " dB";
}

TEST(TextureChannelAnalysis, BC4SurvivesTheContainerRoundTripAtItsOwnBlockSize)
{
    constexpr u32 kW = 16;
    constexpr u32 kH = 16;
    const CompressedTextureImage original =
        TextureCompression::EncodeBC4(MakeGreyscaleRGBA(kW, kH, false).data(), kW, kH, 4, true);
    ASSERT_TRUE(original.IsValid());

    CompressedTextureImage restored;
    ASSERT_TRUE(TextureCompression::DeserializeFromBlob(TextureCompression::SerializeToBlob(original), restored));
    EXPECT_EQ(restored.Format, TextureCompressionFormat::BC4);
    ASSERT_EQ(restored.MipLevels(), original.MipLevels());
    for (u32 level = 0; level < original.MipLevels(); ++level)
        EXPECT_EQ(restored.Mips[level], original.Mips[level]) << "mip " << level << " block bytes differ";
}

TEST(TextureChannelAnalysis, AutoCookNarrowsGreyscaleToBC4)
{
    // The item-5 payoff: a greyscale mask / AO / roughness map cooks to half the bytes,
    // chosen from the pixels, with no filename involved.
    constexpr u32 kW = 32;
    constexpr u32 kH = 32;
    const std::filesystem::path png = WritePng("channels_grey.png", MakeGreyscaleRGBA(kW, kH, false), kW, kH, 3);

    TextureCompression::CompressOptions options; // Format = None -> auto
    CompressedTextureImage image;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, image));
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC4);
    EXPECT_FALSE(image.HasAlpha);

    Remove(png);
}

TEST(TextureChannelAnalysis, AutoCookKeepsColourAndRealAlphaOnBC7)
{
    constexpr u32 kW = 32;
    constexpr u32 kH = 32;
    const std::filesystem::path colorPng = WritePng("channels_color.png", MakeColorRGBA(kW, kH, false), kW, kH, 3);
    const std::filesystem::path alphaPng = WritePng("channels_alpha.png", MakeGreyscaleRGBA(kW, kH, true), kW, kH, 4);

    TextureCompression::CompressOptions options;
    CompressedTextureImage colorImage;
    ASSERT_TRUE(TextureCompression::CompressImageFile(colorPng.string(), options, colorImage));
    EXPECT_EQ(colorImage.Format, TextureCompressionFormat::BC7) << "colour must not be narrowed";

    // Greyscale but with real alpha: BC4 cannot carry the alpha, so it stays BC7.
    CompressedTextureImage alphaImage;
    ASSERT_TRUE(TextureCompression::CompressImageFile(alphaPng.string(), options, alphaImage));
    EXPECT_EQ(alphaImage.Format, TextureCompressionFormat::BC7);
    EXPECT_TRUE(alphaImage.HasAlpha);

    Remove(colorPng);
    Remove(alphaPng);
}

TEST(TextureChannelAnalysis, OpaqueFourChannelSourceIsNotReportedAsTransparent)
{
    // Before the alpha was measured, HasAlpha was `channels == 4`, so any RGBA PNG with a
    // constant 255 alpha claimed transparency and its material sorted into the
    // transparent pass. This is the regression test for that, not a new feature.
    constexpr u32 kW = 16;
    constexpr u32 kH = 16;
    const std::filesystem::path png = WritePng("channels_opaque_rgba.png", MakeColorRGBA(kW, kH, false), kW, kH, 4);

    TextureCompression::CompressOptions options;
    CompressedTextureImage image;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, image));
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC7);
    EXPECT_FALSE(image.HasAlpha) << "a 4-channel source with a constant 255 alpha is opaque";

    Remove(png);
}

TEST(TextureChannelAnalysis, NarrowingRGBToTwoChannelsStaysAnExplicitChoice)
{
    // BC5 decodes blue as 0. Even when a source's blue is constant, dropping it changes
    // what a shader reads unless that constant WAS 0 — so the cook never picks BC5 by
    // itself, and the sidecar is the only way in. This is the boundary of item 5, pinned.
    constexpr u32 kW = 16;
    constexpr u32 kH = 16;
    std::vector<u8> rg = MakeColorRGBA(kW, kH, false);
    for (sizet i = 0; i < static_cast<sizet>(kW) * kH; ++i)
        rg[i * 4 + 2] = 200; // a constant, non-zero blue

    const std::filesystem::path png = WritePng("channels_rg_constblue.png", rg, kW, kH, 3);

    TextureCompression::CompressOptions options;
    CompressedTextureImage automatic;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, automatic));
    EXPECT_EQ(automatic.Format, TextureCompressionFormat::BC7)
        << "a constant blue is still a blue the shader can read; the cook must not drop it";

    Remove(png);
}

TEST(TextureChannelAnalysis, UniformlyTranslucentSourceIsNotCalledOpaqueAndIsNotNarrowed)
{
    // "Has alpha" has to mean NOT OPAQUE, not "alpha is non-constant". A window pane at a
    // flat 50 % is entirely constant in alpha and entirely translucent, and getting this
    // wrong is doubly bad here: it would mis-sort the material AND, because the RGB is
    // greyscale, hand it to the BC4 rule, which drops the alpha channel for good.
    constexpr u32 kW = 16;
    constexpr u32 kH = 16;
    std::vector<u8> pixels(static_cast<sizet>(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y)
    {
        for (u32 x = 0; x < kW; ++x)
        {
            const auto value = static_cast<u8>((x * 255) / std::max(1u, kW - 1));
            u8* p = &pixels[(static_cast<sizet>(y) * kW + x) * 4];
            p[0] = value;
            p[1] = value;
            p[2] = value;
            p[3] = 128; // constant, and very much alpha
        }
    }

    const TextureCompression::ChannelUsage usage =
        TextureCompression::AnalyzeChannels(pixels.data(), kW, kH, 4);
    EXPECT_TRUE(usage.IsGreyscale);
    EXPECT_TRUE(usage.HasAlpha) << "a constant 128 alpha is still alpha";

    const std::filesystem::path png = WritePng("channels_translucent.png", pixels, kW, kH, 4);
    TextureCompression::CompressOptions options;
    CompressedTextureImage image;
    ASSERT_TRUE(TextureCompression::CompressImageFile(png.string(), options, image));
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC7)
        << "greyscale RGB must not narrow to BC4 while the source carries alpha";
    EXPECT_TRUE(image.HasAlpha);

    Remove(png);
}
