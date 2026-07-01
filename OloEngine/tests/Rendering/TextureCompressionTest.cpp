// OLO_TEST_LAYER: L3
//
// Data round-trip contract for the offline BC7/BC5 texture-compression cook (#440).
// Pure CPU — no GL context required, so these run in CI. They pin: block geometry
// math, encode->decode fidelity (via PSNR, since BCn is lossy — never bit-exact),
// mip-chain generation, the .olotex container round-trip (bit-exact block bytes),
// and non-multiple-of-4 (partial block) handling. The GPU upload path is verified
// separately by the GL evidence test.

#include "OloEngine/Renderer/TextureCompression.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // Peak signal-to-noise ratio (dB) between two equal-length 8-bit buffers over the
    // given channel count, sampling only `compareChannels` of each `stride`-byte texel.
    // Returns a large finite value when the buffers are identical.
    double ComputePSNR(const std::vector<u8>& a, const std::vector<u8>& b, u32 stride, u32 compareChannels)
    {
        EXPECT_EQ(a.size(), b.size());
        if (a.size() != b.size() || a.empty())
            return 0.0;

        double mse = 0.0;
        sizet samples = 0;
        for (sizet texel = 0; texel + stride <= a.size(); texel += stride)
        {
            for (u32 c = 0; c < compareChannels; ++c)
            {
                const double diff = static_cast<double>(a[texel + c]) - static_cast<double>(b[texel + c]);
                mse += diff * diff;
                ++samples;
            }
        }
        if (samples == 0)
            return 0.0;
        mse /= static_cast<double>(samples);
        if (mse < 1e-9)
            return 99.0; // effectively lossless
        return 10.0 * std::log10((255.0 * 255.0) / mse);
    }

    // A smooth RGBA gradient — BCn compresses smooth data very well, so a healthy
    // encoder should clear a comfortable PSNR margin.
    std::vector<u8> MakeGradientRGBA(u32 width, u32 height)
    {
        std::vector<u8> pixels(static_cast<sizet>(width) * height * 4);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                u8* p = &pixels[(static_cast<sizet>(y) * width + x) * 4];
                p[0] = static_cast<u8>((x * 255) / std::max(1u, width - 1));
                p[1] = static_cast<u8>((y * 255) / std::max(1u, height - 1));
                p[2] = static_cast<u8>(((x + y) * 255) / std::max(1u, (width + height) - 2));
                p[3] = 255;
            }
        }
        return pixels;
    }

    // A tangent-space-normal-like RG field: xy sweep, packed into R,G (B,A unused here).
    std::vector<u8> MakeNormalRG(u32 width, u32 height)
    {
        std::vector<u8> pixels(static_cast<sizet>(width) * height * 2);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                u8* p = &pixels[(static_cast<sizet>(y) * width + x) * 2];
                p[0] = static_cast<u8>((x * 255) / std::max(1u, width - 1));
                p[1] = static_cast<u8>((y * 255) / std::max(1u, height - 1));
            }
        }
        return pixels;
    }

    std::filesystem::path CaseKeyedTempFile(const char* suffix)
    {
        const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
        std::string name = std::string(info->test_suite_name()) + "." + info->name() + suffix;
        return std::filesystem::temp_directory_path() / name;
    }
} // namespace

TEST(TextureCompression, BlockGeometry)
{
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::BC7), 16u);
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::BC5), 16u);
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::None), 0u);

    // ceil(dim/4), at least one block.
    EXPECT_EQ(TextureCompression::BlockCount(1), 1u);
    EXPECT_EQ(TextureCompression::BlockCount(4), 1u);
    EXPECT_EQ(TextureCompression::BlockCount(5), 2u);
    EXPECT_EQ(TextureCompression::BlockCount(16), 4u);
    EXPECT_EQ(TextureCompression::BlockCount(17), 5u);

    // 16x16 -> 4x4 blocks -> 16 blocks * 16 bytes = 256.
    EXPECT_EQ(TextureCompression::MipByteSize(TextureCompressionFormat::BC7, 16, 16), 256u);
    // 13x7 -> ceil(13/4)=4, ceil(7/4)=2 -> 8 blocks * 16 = 128.
    EXPECT_EQ(TextureCompression::MipByteSize(TextureCompressionFormat::BC5, 13, 7), 128u);
}

TEST(TextureCompression, EncodeBC7RoundTripWithinTolerance)
{
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);

    const CompressedTextureImage image = TextureCompression::EncodeBC7(source.data(), kW, kH, 4, false, false);
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC7);
    EXPECT_EQ(image.MipLevels(), 1u); // generateMips=false
    EXPECT_EQ(image.Mips[0].size(), TextureCompression::MipByteSize(TextureCompressionFormat::BC7, kW, kH));

    std::vector<u8> decoded;
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBA8(image, 0, decoded, dw, dh));
    EXPECT_EQ(dw, kW);
    EXPECT_EQ(dh, kH);

    // Compare RGB (BC7 encodes A too, but the source A is a constant 255).
    const double psnr = ComputePSNR(source, decoded, 4, 3);
    EXPECT_GT(psnr, 35.0) << "BC7 gradient round-trip PSNR too low: " << psnr << " dB";
}

TEST(TextureCompression, EncodeBC5RoundTripWithinTolerance)
{
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const std::vector<u8> source = MakeNormalRG(kW, kH); // 2 channels

    const CompressedTextureImage image = TextureCompression::EncodeBC5(source.data(), kW, kH, 2, false);
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC5);

    std::vector<u8> decoded; // RGBA8: R,G carry the two channels, B=0, A=255
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBA8(image, 0, decoded, dw, dh));

    // Re-pack the source RG as RGBA to reuse the PSNR helper on channels 0..1.
    std::vector<u8> sourceRGBA(static_cast<sizet>(kW) * kH * 4, 0);
    for (sizet i = 0; i < static_cast<sizet>(kW) * kH; ++i)
    {
        sourceRGBA[i * 4 + 0] = source[i * 2 + 0];
        sourceRGBA[i * 4 + 1] = source[i * 2 + 1];
        sourceRGBA[i * 4 + 3] = 255;
    }
    const double psnr = ComputePSNR(sourceRGBA, decoded, 4, 2);
    EXPECT_GT(psnr, 35.0) << "BC5 normal round-trip PSNR too low: " << psnr << " dB";
}

TEST(TextureCompression, GeneratesFullMipChain)
{
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);

    const CompressedTextureImage image = TextureCompression::EncodeBC7(source.data(), kW, kH, 4, true, true);
    ASSERT_TRUE(image.IsValid());
    // 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1  == 7 levels (floor(log2(64)) + 1).
    EXPECT_EQ(image.MipLevels(), 7u);

    for (u32 level = 0; level < image.MipLevels(); ++level)
    {
        const u32 mw = std::max(1u, kW >> level);
        const u32 mh = std::max(1u, kH >> level);
        EXPECT_EQ(image.Mips[level].size(), TextureCompression::MipByteSize(TextureCompressionFormat::BC7, mw, mh))
            << "mip " << level << " byte size mismatch";
    }
}

TEST(TextureCompression, HandlesNonMultipleOf4Dimensions)
{
    constexpr u32 kW = 13; // deliberately not a multiple of 4
    constexpr u32 kH = 7;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);

    const CompressedTextureImage image = TextureCompression::EncodeBC7(source.data(), kW, kH, 4, false, false);
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.Width, kW);
    EXPECT_EQ(image.Height, kH);

    std::vector<u8> decoded;
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBA8(image, 0, decoded, dw, dh));
    EXPECT_EQ(dw, kW);
    EXPECT_EQ(dh, kH);
    EXPECT_EQ(decoded.size(), static_cast<sizet>(kW) * kH * 4);

    const double psnr = ComputePSNR(source, decoded, 4, 3);
    EXPECT_GT(psnr, 30.0) << "partial-block PSNR too low: " << psnr << " dB";
}

TEST(TextureCompression, ContainerBlobRoundTripIsBitExact)
{
    constexpr u32 kW = 32;
    constexpr u32 kH = 16;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);

    const CompressedTextureImage original = TextureCompression::EncodeBC7(source.data(), kW, kH, 4, true, true);
    ASSERT_TRUE(original.IsValid());

    const std::vector<u8> blob = TextureCompression::SerializeToBlob(original);
    ASSERT_FALSE(blob.empty());

    CompressedTextureImage restored;
    ASSERT_TRUE(TextureCompression::DeserializeFromBlob(blob, restored));

    EXPECT_EQ(restored.Format, original.Format);
    EXPECT_EQ(restored.Width, original.Width);
    EXPECT_EQ(restored.Height, original.Height);
    EXPECT_EQ(restored.SRGB, original.SRGB);
    ASSERT_EQ(restored.MipLevels(), original.MipLevels());
    for (u32 level = 0; level < original.MipLevels(); ++level)
        EXPECT_EQ(restored.Mips[level], original.Mips[level]) << "mip " << level << " block bytes differ";
}

TEST(TextureCompression, SRGBFlagSurvivesContainerRoundTrip)
{
    constexpr u32 kW = 8;
    constexpr u32 kH = 8;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);

    const CompressedTextureImage srgbImage = TextureCompression::EncodeBC7(source.data(), kW, kH, 4, /*srgb*/ true, false);
    ASSERT_TRUE(srgbImage.SRGB);

    CompressedTextureImage restored;
    ASSERT_TRUE(TextureCompression::DeserializeFromBlob(TextureCompression::SerializeToBlob(srgbImage), restored));
    EXPECT_TRUE(restored.SRGB);
}

TEST(TextureCompression, RejectsCorruptBlob)
{
    // Empty and bad-magic blobs must be rejected, not crash.
    CompressedTextureImage out;
    EXPECT_FALSE(TextureCompression::DeserializeFromBlob({}, out));

    std::vector<u8> garbage = { 'N', 'O', 'P', 'E', 1, 2, 3, 4 };
    EXPECT_FALSE(TextureCompression::DeserializeFromBlob(garbage, out));
}

TEST(TextureCompression, OfflineCookFromPngProducesLoadableOloTex)
{
    // Full offline-cook primitive: write a source PNG, CompressTextureFile it to a
    // .olotex, then read the container back and confirm a valid BC7 mip chain.
    constexpr u32 kW = 32;
    constexpr u32 kH = 24;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);

    const std::filesystem::path pngPath = CaseKeyedTempFile(".png");
    const std::filesystem::path oloTexPath = CaseKeyedTempFile(".olotex");
    ASSERT_NE(::stbi_write_png(pngPath.string().c_str(), static_cast<int>(kW), static_cast<int>(kH), 4,
                               source.data(), static_cast<int>(kW) * 4),
              0);

    TextureCompression::CompressOptions options;
    options.Format = TextureCompressionFormat::BC7;
    options.GenerateMips = true;
    ASSERT_TRUE(TextureCompression::CompressTextureFile(pngPath.string(), oloTexPath.string(), options));

    CompressedTextureImage loaded;
    ASSERT_TRUE(TextureCompression::ReadFile(oloTexPath.string(), loaded));
    EXPECT_EQ(loaded.Format, TextureCompressionFormat::BC7);
    EXPECT_EQ(loaded.Width, kW);
    EXPECT_EQ(loaded.Height, kH);
    EXPECT_GT(loaded.MipLevels(), 1u);

    // The .olotex should be materially smaller than the raw RGBA8 source it came from.
    sizet compressedBytes = 0;
    for (const auto& mip : loaded.Mips)
        compressedBytes += mip.size();
    EXPECT_LT(compressedBytes, source.size()) << "compressed size should be below raw RGBA8";

    std::error_code ec;
    std::filesystem::remove(pngPath, ec);
    std::filesystem::remove(oloTexPath, ec);
}

TEST(TextureCompression, WriteThenReadFileRoundTrip)
{
    constexpr u32 kW = 16;
    constexpr u32 kH = 16;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);
    const CompressedTextureImage original = TextureCompression::EncodeBC7(source.data(), kW, kH, 4, false, true);
    ASSERT_TRUE(original.IsValid());

    const std::filesystem::path path = CaseKeyedTempFile(".olotex");
    ASSERT_TRUE(TextureCompression::WriteFile(path.string(), original));

    CompressedTextureImage restored;
    ASSERT_TRUE(TextureCompression::ReadFile(path.string(), restored));
    EXPECT_EQ(restored.Width, original.Width);
    EXPECT_EQ(restored.Height, original.Height);
    ASSERT_EQ(restored.MipLevels(), original.MipLevels());
    EXPECT_EQ(restored.Mips[0], original.Mips[0]);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
