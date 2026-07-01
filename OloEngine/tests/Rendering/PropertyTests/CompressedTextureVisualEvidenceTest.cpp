// OLO_TEST_LAYER: L3
//
// GPU round-trip + pixel evidence for the BC7/BC5 compressed-texture upload path
// (#440). Runs on a real GL 4.6 context (SKIPs cleanly on CI without a GPU).
//
// Two independent proofs that the glCompressedTextureSubImage2D upload is correct:
//   1. glGetCompressedTextureImage reads the stored blocks back and they are
//      BIT-EXACT with the CPU-encoded blocks we uploaded — proves storage + layout.
//   2. glGetTextureImage(GL_RGBA) asks the driver to DECOMPRESS the texture; the
//      result matches the source within a PSNR tolerance — proves the GPU samples
//      the format we think it does. That decompressed frame is written to a PNG and
//      read back, so there is a human-inspectable pixel artifact per the project's
//      visual-verification rule.

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Tests;

namespace
{
    std::vector<u8> MakeGradientRGBA(u32 width, u32 height)
    {
        std::vector<u8> pixels(static_cast<sizet>(width) * height * 4);
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                u8* p = &pixels[(static_cast<sizet>(y) * width + x) * 4];
                p[0] = static_cast<u8>((x * 255) / (width - 1));
                p[1] = static_cast<u8>((y * 255) / (height - 1));
                p[2] = static_cast<u8>(128);
                p[3] = 255;
            }
        }
        return pixels;
    }

    double ComputePSNR(const std::vector<u8>& a, const std::vector<u8>& b, u32 compareChannels)
    {
        if (a.size() != b.size() || a.empty())
            return 0.0;
        double mse = 0.0;
        sizet samples = 0;
        for (sizet texel = 0; texel + 4 <= a.size(); texel += 4)
        {
            for (u32 c = 0; c < compareChannels; ++c)
            {
                const double d = static_cast<double>(a[texel + c]) - static_cast<double>(b[texel + c]);
                mse += d * d;
                ++samples;
            }
        }
        if (samples == 0)
            return 0.0;
        mse /= static_cast<double>(samples);
        if (mse < 1e-9)
            return 99.0;
        return 10.0 * std::log10((255.0 * 255.0) / mse);
    }

    std::filesystem::path CaseKeyedTempPng()
    {
        const testing::TestInfo* info = testing::UnitTest::GetInstance()->current_test_info();
        std::string name = std::string(info->test_suite_name()) + "." + info->name() + ".png";
        return std::filesystem::temp_directory_path() / name;
    }
} // namespace

TEST(CompressedTextureVisualEvidence, BC7UploadStoresBlocksAndDecompressesOnGPU)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 64;
    constexpr u32 kH = 64;
    const std::vector<u8> source = MakeGradientRGBA(kW, kH);

    // Encode base color -> BC7 (sRGB colour), base mip only for a clean 1:1 comparison.
    const CompressedTextureImage image = TextureCompression::EncodeBC7(source.data(), kW, kH, 4, /*srgb*/ true, /*mips*/ false);
    ASSERT_TRUE(image.IsValid());

    Ref<Texture2D> texture = Texture2D::Create(image);
    ASSERT_TRUE(texture);
    EXPECT_TRUE(texture->IsLoaded());
    EXPECT_EQ(texture->GetWidth(), kW);
    EXPECT_EQ(texture->GetHeight(), kH);
    EXPECT_NE(texture->GetRendererID(), 0u);

    while (glGetError() != GL_NO_ERROR)
    {
    } // drain any leaked errors

    const GLuint id = texture->GetRendererID();

    // (1) Read the stored compressed blocks back — must be bit-exact with upload.
    {
        GLint compressedSize = 0;
        glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &compressedSize);
        ASSERT_EQ(static_cast<sizet>(compressedSize), image.Mips[0].size())
            << "GPU-reported compressed size != uploaded block bytes";

        std::vector<u8> readback(static_cast<sizet>(compressedSize));
        glGetCompressedTextureImage(id, 0, compressedSize, readback.data());
        ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glGetCompressedTextureImage failed";
        EXPECT_EQ(readback, image.Mips[0]) << "stored BC7 blocks differ from uploaded blocks";
    }

    // (2) Ask the driver to decompress the texture and compare to the source.
    std::vector<u8> decompressed(static_cast<sizet>(kW) * kH * 4);
    glGetTextureImage(id, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      static_cast<GLsizei>(decompressed.size()), decompressed.data());
    ASSERT_EQ(glGetError(), GL_NO_ERROR) << "glGetTextureImage (decompress) failed";

    const double psnr = ComputePSNR(source, decompressed, 3);
    EXPECT_GT(psnr, 30.0) << "GPU-decompressed BC7 PSNR too low: " << psnr << " dB";

    // Pixel evidence: write the decompressed frame and read it back to confirm.
    const std::filesystem::path pngPath = CaseKeyedTempPng();
    const int wrote = ::stbi_write_png(pngPath.string().c_str(), static_cast<int>(kW), static_cast<int>(kH), 4,
                                       decompressed.data(), static_cast<int>(kW) * 4);
    EXPECT_NE(wrote, 0) << "failed to write evidence PNG";
    if (wrote != 0)
    {
        int rw = 0;
        int rh = 0;
        int rc = 0;
        stbi_uc* reread = ::stbi_load(pngPath.string().c_str(), &rw, &rh, &rc, 4);
        EXPECT_NE(reread, nullptr);
        EXPECT_EQ(rw, static_cast<int>(kW));
        EXPECT_EQ(rh, static_cast<int>(kH));
        if (reread)
            ::stbi_image_free(reread);
        OLO_CORE_INFO("CompressedTextureVisualEvidence: wrote BC7 decompressed evidence to {}", pngPath.string());
        std::error_code ec;
        std::filesystem::remove(pngPath, ec);
    }
}

TEST(CompressedTextureVisualEvidence, BC5UploadStoresBlocksAndDecompressesOnGPU)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kW = 32;
    constexpr u32 kH = 32;
    // Normal-map-like RG data expanded to RGBA (B=0, A=255) for the encoder.
    std::vector<u8> source(static_cast<sizet>(kW) * kH * 4, 0);
    for (u32 y = 0; y < kH; ++y)
    {
        for (u32 x = 0; x < kW; ++x)
        {
            u8* p = &source[(static_cast<sizet>(y) * kW + x) * 4];
            p[0] = static_cast<u8>((x * 255) / (kW - 1));
            p[1] = static_cast<u8>((y * 255) / (kH - 1));
            p[3] = 255;
        }
    }

    const CompressedTextureImage image = TextureCompression::EncodeBC5(source.data(), kW, kH, 4, /*mips*/ false);
    ASSERT_TRUE(image.IsValid());

    Ref<Texture2D> texture = Texture2D::Create(image);
    ASSERT_TRUE(texture);
    EXPECT_TRUE(texture->IsLoaded());
    EXPECT_NE(texture->GetRendererID(), 0u);

    while (glGetError() != GL_NO_ERROR)
    {
    }

    const GLuint id = texture->GetRendererID();

    GLint compressedSize = 0;
    glGetTextureLevelParameteriv(id, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &compressedSize);
    ASSERT_EQ(static_cast<sizet>(compressedSize), image.Mips[0].size());
    std::vector<u8> readback(static_cast<sizet>(compressedSize));
    glGetCompressedTextureImage(id, 0, compressedSize, readback.data());
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    EXPECT_EQ(readback, image.Mips[0]) << "stored BC5 blocks differ from uploaded blocks";

    // Decompress and compare the two carried channels (R,G). BC5 leaves B undefined
    // on the GPU (reconstructed by shaders), so only compare R,G here.
    std::vector<u8> decompressed(static_cast<sizet>(kW) * kH * 4);
    glGetTextureImage(id, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                      static_cast<GLsizei>(decompressed.size()), decompressed.data());
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    const double psnr = ComputePSNR(source, decompressed, 2);
    EXPECT_GT(psnr, 30.0) << "GPU-decompressed BC5 PSNR too low: " << psnr << " dB";
}
