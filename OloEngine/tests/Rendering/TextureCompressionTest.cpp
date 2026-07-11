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

    // ---- BC6H (HDR) helpers -----------------------------------------------
    // A smooth HDR RGB gradient spanning a wide dynamic range (dark ~0.02 up to `peak`),
    // so the round-trip exercises BC6H's log-ish half-float encoding across bright and
    // dark regions — not just the [0,1] band an LDR test would cover.
    std::vector<f32> MakeGradientHDR(u32 width, u32 height, f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(width) * height * 3);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const f32 fx = static_cast<f32>(x) / std::max(1u, width - 1);
                const f32 fy = static_cast<f32>(y) / std::max(1u, height - 1);
                f32* p = &pixels[(static_cast<sizet>(y) * width + x) * 3];
                // Different curve per channel; +0.02 keeps values off exact zero so the
                // relative-error view is well behaved.
                p[0] = 0.02f + peak * fx * fx;          // red ramps quadratically
                p[1] = 0.02f + peak * fy;               // green ramps linearly
                p[2] = 0.02f + peak * (fx * fy) * 0.5f; // blue: product term
            }
        }
        return pixels;
    }

    // Peak-normalized PSNR (dB) over `compareChannels` of each `stride`-float texel.
    double ComputePSNRFloat(const std::vector<f32>& a, const std::vector<f32>& b, u32 stride, u32 compareChannels, double peak)
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
        if (mse < 1e-12)
            return 99.0;
        return 10.0 * std::log10((peak * peak) / mse);
    }
} // namespace

TEST(TextureCompression, BlockGeometry)
{
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::BC7), 16u);
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::BC5), 16u);
    EXPECT_EQ(TextureCompression::BlockSizeBytes(TextureCompressionFormat::BC6H), 16u);
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
    EXPECT_EQ(restored.HasAlpha, original.HasAlpha);
    ASSERT_EQ(restored.MipLevels(), original.MipLevels());
    for (u32 level = 0; level < original.MipLevels(); ++level)
        EXPECT_EQ(restored.Mips[level], original.Mips[level]) << "mip " << level << " block bytes differ";
}

TEST(TextureCompression, BC7RecordsAlphaFromSourceChannelCount)
{
    constexpr u32 kW = 8;
    constexpr u32 kH = 8;
    // 4-channel source -> alpha present.
    const std::vector<u8> rgba = MakeGradientRGBA(kW, kH);
    const CompressedTextureImage withAlpha = TextureCompression::EncodeBC7(rgba.data(), kW, kH, 4, false, false);
    EXPECT_TRUE(withAlpha.HasAlpha);

    // 3-channel source -> opaque, must NOT report alpha (else opaque albedo is
    // mis-sorted into the transparent pass).
    std::vector<u8> rgb(static_cast<sizet>(kW) * kH * 3);
    for (sizet i = 0; i < rgb.size(); ++i)
        rgb[i] = static_cast<u8>(i);
    const CompressedTextureImage noAlpha = TextureCompression::EncodeBC7(rgb.data(), kW, kH, 3, false, false);
    EXPECT_FALSE(noAlpha.HasAlpha);
}

TEST(TextureCompression, BC5RejectsSingleChannelSource)
{
    constexpr u32 kW = 8;
    constexpr u32 kH = 8;
    std::vector<u8> gray(static_cast<sizet>(kW) * kH, 128);
    // 1 channel is meaningless for a two-channel (R,G) format — must be rejected.
    const CompressedTextureImage image = TextureCompression::EncodeBC5(gray.data(), kW, kH, 1, false);
    EXPECT_FALSE(image.IsValid());
}

TEST(TextureCompression, DeserializeRejectsHostileHeaderFields)
{
    // Build a valid blob, then corrupt the header fields to hostile values and confirm
    // deserialize rejects them BEFORE any large allocation (mipCount / dimensions).
    const std::vector<u8> source = MakeGradientRGBA(16, 16);
    const CompressedTextureImage good = TextureCompression::EncodeBC7(source.data(), 16, 16, 4, false, false);
    std::vector<u8> blob = TextureCompression::SerializeToBlob(good);
    ASSERT_GE(blob.size(), 28u);

    // Header layout (little-endian u32 after the 4-byte magic):
    // [4]=version [8]=format [12]=width [16]=height [20]=flags [24]=mipCount
    const auto patchU32 = [](std::vector<u8>& b, sizet off, u32 v)
    {
        b[off + 0] = static_cast<u8>(v & 0xFF);
        b[off + 1] = static_cast<u8>((v >> 8) & 0xFF);
        b[off + 2] = static_cast<u8>((v >> 16) & 0xFF);
        b[off + 3] = static_cast<u8>((v >> 24) & 0xFF);
    };

    {
        std::vector<u8> hostile = blob;
        patchU32(hostile, 24, 0xFFFFFFFFu); // absurd mipCount -> must not reserve ~100GB
        CompressedTextureImage out;
        EXPECT_FALSE(TextureCompression::DeserializeFromBlob(hostile, out));
    }
    {
        std::vector<u8> hostile = blob;
        patchU32(hostile, 12, 0xFFFFFFFEu); // absurd width
        CompressedTextureImage out;
        EXPECT_FALSE(TextureCompression::DeserializeFromBlob(hostile, out));
    }
    {
        std::vector<u8> hostile = blob;
        patchU32(hostile, 20, 0x4u); // flags: an undefined bit set -> non-canonical
        CompressedTextureImage out;
        EXPECT_FALSE(TextureCompression::DeserializeFromBlob(hostile, out));
    }
    {
        std::vector<u8> trailing = blob;
        trailing.push_back(0x00); // extra byte after the final mip -> not fully consumed
        CompressedTextureImage out;
        EXPECT_FALSE(TextureCompression::DeserializeFromBlob(trailing, out));
    }
}

TEST(TextureCompression, DeserializeRejectsBC5WithColorFlags)
{
    // BC5 (two-channel normal data) must carry neither sRGB nor alpha. A blob that
    // claims otherwise is corrupt/version-skewed and must be rejected.
    constexpr u32 kW = 8;
    constexpr u32 kH = 8;
    std::vector<u8> rg(static_cast<sizet>(kW) * kH * 2);
    for (sizet i = 0; i < rg.size(); ++i)
        rg[i] = static_cast<u8>(i * 3);
    const CompressedTextureImage bc5 = TextureCompression::EncodeBC5(rg.data(), kW, kH, 2, false);
    ASSERT_TRUE(bc5.IsValid());
    std::vector<u8> blob = TextureCompression::SerializeToBlob(bc5);
    ASSERT_GE(blob.size(), 28u);

    // Patch the flags field (offset 20) to set the sRGB bit — illegal for BC5.
    blob[20] = 0x1u;
    CompressedTextureImage out;
    EXPECT_FALSE(TextureCompression::DeserializeFromBlob(blob, out));
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

// ---- BC6H (HDR) tests -----------------------------------------------------
// These validate the from-scratch BC6H mode-11 encoder END TO END through the
// vendored bcdec reference decoder — an ORACLE independent of our encoder, so a
// wrong bit layout / quantization can't hide behind a matched CPU decoder. A wrong
// mode value or bit packing would tank the PSNR and fail here (and, redundantly, the
// GPU visual-evidence test decodes the same blocks in hardware).

TEST(TextureCompression, EncodeBC6HRoundTripWithinTolerance)
{
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    constexpr f32 kPeak = 8.0f;
    const std::vector<f32> source = MakeGradientHDR(kW, kH, kPeak);

    const CompressedTextureImage image = TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, false);
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.Format, TextureCompressionFormat::BC6H);
    EXPECT_FALSE(image.SRGB);
    EXPECT_FALSE(image.HasAlpha);
    EXPECT_EQ(image.MipLevels(), 1u); // generateMips=false
    EXPECT_EQ(image.Mips[0].size(), TextureCompression::MipByteSize(TextureCompressionFormat::BC6H, kW, kH));

    std::vector<f32> decoded; // RGBA float: R,G,B and A=1
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBAFloat(image, 0, decoded, dw, dh));
    EXPECT_EQ(dw, kW);
    EXPECT_EQ(dh, kH);

    // Re-pack source RGB as RGBA (A=1) so the stride-4 PSNR helper lines up with decoded.
    std::vector<f32> sourceRGBA(static_cast<sizet>(kW) * kH * 4, 1.0f);
    for (sizet i = 0; i < static_cast<sizet>(kW) * kH; ++i)
    {
        sourceRGBA[i * 4 + 0] = source[i * 3 + 0];
        sourceRGBA[i * 4 + 1] = source[i * 3 + 1];
        sourceRGBA[i * 4 + 2] = source[i * 3 + 2];
    }
    // ~38 dB peak-normalized on this deliberately-curved gradient (the red channel ramps
    // quadratically, the worst case for mode-11's single linear segment per block). Real
    // IBL/environment content is smoother per-block and scores higher. 35 dB is a strong
    // correctness floor — a wrong bit layout / quantization tanks this below ~15 dB — with
    // margin; higher fidelity would need the multi-subset BC6H modes (a deferred encoder
    // follow-up). The flat-block test above pins near-lossless behaviour separately.
    const double psnr = ComputePSNRFloat(sourceRGBA, decoded, 4, 3, kPeak);
    EXPECT_GT(psnr, 35.0) << "BC6H HDR gradient round-trip PSNR too low: " << psnr << " dB";
}

TEST(TextureCompression, BC6HFlatBlockIsNearLossless)
{
    // A constant-color HDR block has coincident endpoints — every index resolves to the
    // same value, so the round-trip should be essentially exact (only the half-float
    // quantization of the single value survives).
    constexpr u32 kW = 4;
    constexpr u32 kH = 4;
    std::vector<f32> source(static_cast<sizet>(kW) * kH * 3);
    for (sizet i = 0; i < static_cast<sizet>(kW) * kH; ++i)
    {
        source[i * 3 + 0] = 3.5f;
        source[i * 3 + 1] = 1.25f;
        source[i * 3 + 2] = 0.5f;
    }
    const CompressedTextureImage image = TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, false);
    ASSERT_TRUE(image.IsValid());

    std::vector<f32> decoded;
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBAFloat(image, 0, decoded, dw, dh));
    for (sizet i = 0; i < static_cast<sizet>(kW) * kH; ++i)
    {
        EXPECT_NEAR(decoded[i * 4 + 0], 3.5f, 0.02f);
        EXPECT_NEAR(decoded[i * 4 + 1], 1.25f, 0.02f);
        EXPECT_NEAR(decoded[i * 4 + 2], 0.5f, 0.02f);
    }
}

TEST(TextureCompression, BC6HGeneratesFullMipChain)
{
    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const std::vector<f32> source = MakeGradientHDR(kW, kH, 4.0f);

    const CompressedTextureImage image = TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, true);
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.MipLevels(), 7u); // 64 -> ... -> 1

    for (u32 level = 0; level < image.MipLevels(); ++level)
    {
        const u32 mw = std::max(1u, kW >> level);
        const u32 mh = std::max(1u, kH >> level);
        EXPECT_EQ(image.Mips[level].size(), TextureCompression::MipByteSize(TextureCompressionFormat::BC6H, mw, mh))
            << "mip " << level << " byte size mismatch";
    }
}

TEST(TextureCompression, BC6HHandlesNonMultipleOf4Dimensions)
{
    constexpr u32 kW = 13; // not a multiple of 4
    constexpr u32 kH = 7;
    constexpr f32 kPeak = 6.0f;
    const std::vector<f32> source = MakeGradientHDR(kW, kH, kPeak);

    const CompressedTextureImage image = TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, false);
    ASSERT_TRUE(image.IsValid());
    EXPECT_EQ(image.Width, kW);
    EXPECT_EQ(image.Height, kH);

    std::vector<f32> decoded;
    u32 dw = 0;
    u32 dh = 0;
    ASSERT_TRUE(TextureCompression::DecodeToRGBAFloat(image, 0, decoded, dw, dh));
    EXPECT_EQ(dw, kW);
    EXPECT_EQ(dh, kH);
    EXPECT_EQ(decoded.size(), static_cast<sizet>(kW) * kH * 4);
}

TEST(TextureCompression, BC6HContainerBlobRoundTripIsBitExact)
{
    constexpr u32 kW = 32;
    constexpr u32 kH = 16;
    const std::vector<f32> source = MakeGradientHDR(kW, kH, 5.0f);

    const CompressedTextureImage original = TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, true);
    ASSERT_TRUE(original.IsValid());

    const std::vector<u8> blob = TextureCompression::SerializeToBlob(original);
    ASSERT_FALSE(blob.empty());

    CompressedTextureImage restored;
    ASSERT_TRUE(TextureCompression::DeserializeFromBlob(blob, restored));
    EXPECT_EQ(restored.Format, TextureCompressionFormat::BC6H);
    EXPECT_EQ(restored.Width, original.Width);
    EXPECT_EQ(restored.Height, original.Height);
    EXPECT_FALSE(restored.SRGB);
    EXPECT_FALSE(restored.HasAlpha);
    ASSERT_EQ(restored.MipLevels(), original.MipLevels());
    for (u32 level = 0; level < original.MipLevels(); ++level)
        EXPECT_EQ(restored.Mips[level], original.Mips[level]) << "mip " << level << " block bytes differ";
}

TEST(TextureCompression, EncodeBC6HRejectsInsufficientChannels)
{
    constexpr u32 kW = 8;
    constexpr u32 kH = 8;
    std::vector<f32> rg(static_cast<sizet>(kW) * kH * 2, 1.0f);
    // BC6H is RGB HDR; a 2-channel source is meaningless and must be rejected.
    const CompressedTextureImage image = TextureCompression::EncodeBC6H(rg.data(), kW, kH, 2, false);
    EXPECT_FALSE(image.IsValid());
}

TEST(TextureCompression, DecodeToRGBA8RejectsBC6H)
{
    // BC6H has no meaningful 8-bit representation — DecodeToRGBA8 must refuse it (rather
    // than mis-decode via the BC5 path) so the fallback routes through the float decode.
    constexpr u32 kW = 8;
    constexpr u32 kH = 8;
    const std::vector<f32> source = MakeGradientHDR(kW, kH, 2.0f);
    const CompressedTextureImage image = TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, false);
    ASSERT_TRUE(image.IsValid());

    std::vector<u8> rgba8;
    u32 dw = 0;
    u32 dh = 0;
    EXPECT_FALSE(TextureCompression::DecodeToRGBA8(image, 0, rgba8, dw, dh));
}

TEST(TextureCompression, DeserializeRejectsBC6HWithColorFlags)
{
    // BC6H (linear HDR RGB) must carry neither sRGB nor alpha.
    constexpr u32 kW = 8;
    constexpr u32 kH = 8;
    const std::vector<f32> source = MakeGradientHDR(kW, kH, 2.0f);
    const CompressedTextureImage bc6h = TextureCompression::EncodeBC6H(source.data(), kW, kH, 3, false);
    ASSERT_TRUE(bc6h.IsValid());
    std::vector<u8> blob = TextureCompression::SerializeToBlob(bc6h);
    ASSERT_GE(blob.size(), 28u);

    // Patch flags (offset 20) to set the alpha bit — illegal for BC6H.
    blob[20] = 0x2u;
    CompressedTextureImage out;
    EXPECT_FALSE(TextureCompression::DeserializeFromBlob(blob, out));
}

TEST(TextureCompression, OfflineCookFromHdrAutoSelectsBC6H)
{
    // Full offline cook of an HDR source: write a .hdr, CompressTextureFile with Format
    // left None (auto), and confirm stbi_is_hdr steered the cook to a BC6H mip chain.
    constexpr u32 kW = 32;
    constexpr u32 kH = 24;
    const std::vector<f32> source = MakeGradientHDR(kW, kH, 4.0f);

    const std::filesystem::path hdrPath = CaseKeyedTempFile(".hdr");
    const std::filesystem::path oloTexPath = CaseKeyedTempFile(".olotex");
    ASSERT_NE(::stbi_write_hdr(hdrPath.string().c_str(), static_cast<int>(kW), static_cast<int>(kH), 3, source.data()), 0);

    TextureCompression::CompressOptions options; // Format=None -> auto
    options.GenerateMips = true;
    ASSERT_TRUE(TextureCompression::CompressTextureFile(hdrPath.string(), oloTexPath.string(), options));

    CompressedTextureImage loaded;
    ASSERT_TRUE(TextureCompression::ReadFile(oloTexPath.string(), loaded));
    EXPECT_EQ(loaded.Format, TextureCompressionFormat::BC6H);
    EXPECT_EQ(loaded.Width, kW);
    EXPECT_EQ(loaded.Height, kH);
    EXPECT_GT(loaded.MipLevels(), 1u);

    std::error_code ec;
    std::filesystem::remove(hdrPath, ec);
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
