// =============================================================================
// DataRoundTripTests.cpp
//
// Layer-3 (Data Round-Trip, doc section 3) tests focused on the GPU-side of
// rendering data. The catalog in the doc covers:
//   - Identity round-trip (CPU → serialize → deserialize → CPU)
//   - GPU round-trip (CPU → upload → readback → CPU)
//   - Full pipeline round-trip (CPU → render → cache → load → upload → readback)
//
// This file covers the GPU round-trip tier. The CPU-only serializer
// round-trips are covered by existing suites (InputActionSerializerTest,
// DialogueTreeSerializationTest, etc.). The full-pipeline tier depends on
// the cubemap cache and is covered indirectly by existing IBL tests.
//
// First two tests:
//   1. RGBA32F texture: upload a pseudo-random float field, read it back,
//      assert bit-identical. Catches format mismatches, pixel-packing bugs,
//      endianness issues.
//   2. RGBA8 texture: same thing at 8-bit to catch GL_UNSIGNED_BYTE vs
//      GL_BYTE confusion and gamma-surface misinterpretation.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/IBLCache.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    TEST(DataRoundTripTest, Rgba32FGpuBitIdentity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 32;
        constexpr u32 H = 16;
        constexpr u32 N = W * H * 4;
        std::vector<f32> src(N);

        // Deterministic pseudo-random pattern with negative, positive, and
        // sub-normal-ish values. Avoid NaN/Inf — those compare weirdly.
        for (u32 i = 0; i < N; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(N - 1);
            src[i] = std::sin(t * 12.3f) * 42.0f + (i % 7 == 0 ? -0.5f : 0.125f);
        }

        u32 tex = CreateFloatTexture2D(W, H, src.data());
        std::vector<f32> dst(N, 0.0f);
        ReadbackRgbaFloat(tex, W, H, dst);

        ASSERT_EQ(dst.size(), src.size());
        // Bit-identical: every float matches exactly. Any mismatch = format
        // bug in the upload / readback path.
        u32 diffs = 0;
        for (u32 i = 0; i < N; ++i)
        {
            if (std::memcmp(&src[i], &dst[i], sizeof(f32)) != 0)
                ++diffs;
        }
        EXPECT_EQ(diffs, 0u) << "RGBA32F round-trip produced " << diffs << " bit-different texels";

        ::glDeleteTextures(1, reinterpret_cast<const GLuint*>(&tex));
    }

    TEST(DataRoundTripTest, Rgba8GpuByteIdentity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 8;
        constexpr u32 H = 8;
        constexpr u32 N = W * H * 4;

        std::vector<u8> src(N);
        for (u32 i = 0; i < N; ++i)
            src[i] = static_cast<u8>((i * 37u + 13u) & 0xFFu);

        GLuint tex = 0;
        ::glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        ::glTextureStorage2D(tex, 1, GL_RGBA8, static_cast<GLsizei>(W), static_cast<GLsizei>(H));
        ::glTextureSubImage2D(tex, 0, 0, 0,
                              static_cast<GLsizei>(W), static_cast<GLsizei>(H), GL_RGBA, GL_UNSIGNED_BYTE, src.data());

        std::vector<u8> dst(N, 0);
        ReadbackRgba8(static_cast<u32>(tex), W, H, dst);

        u32 diffs = 0;
        for (u32 i = 0; i < N; ++i)
            if (src[i] != dst[i])
                ++diffs;
        EXPECT_EQ(diffs, 0u) << "RGBA8 round-trip produced " << diffs << " different bytes";

        ::glDeleteTextures(1, &tex);
    }

    // =========================================================================
    // IBL cache round-trip: save a procedurally generated prefilter cubemap
    // with a KNOWN distinct bit pattern per mip level, reload it, and verify
    // that every mip level is preserved bit-exact.
    //
    // This is the exact shape of the bug we shipped previously:
    // IBLCache::LoadCubemapFromCache only loaded mip 0, causing prefilter
    // mips 1-4 to be silently re-generated as bilinear downsamples (via
    // glGenerateTextureMipmap) instead of the proper GGX-convolved mips.
    //
    // We build a RGBA16F 32x32 cubemap with 3 mip levels, fill each mip of
    // each face with a distinct linear-encoded byte pattern, save through
    // IBLCache::Save, then TryLoad, then read back every mip of every face.
    // The BRDF LUT and irradiance map are reused as cheap stand-ins since
    // Save expects a full triplet; their content isn't under test here.
    //
    // Encoded pattern per (mip, face) is bit-exact: texel_value =
    // float(mip * 100 + face * 10 + channel) / 1000.0, quantized through
    // fp16 storage. Readback compares against fp16-roundtripped expected
    // values to avoid false negatives from precision loss at storage.
    // =========================================================================
    TEST(DataRoundTripTest, IblCacheCubemapRoundTripPreservesAllMips)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kWidth = 32;
        constexpr u32 kHeight = 32;
        constexpr u32 kMipLevels = 3;

        // Use a temp cache directory so we don't stomp on the real one.
        auto tempDir = std::filesystem::temp_directory_path() / "olo_ibl_cache_test";
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
        IBLCache::Initialize(tempDir);

        // -- Build the test prefilter cubemap with a distinct pattern per mip.
        // Use RGBA32F so upload / readback is exact bit-for-bit (no half-float
        // conversion in the path). The bug this test catches is structural
        // (missing mip levels), not format-precision; RGBA32F is sufficient.
        CubemapSpecification spec{};
        spec.Width = kWidth;
        spec.Height = kHeight;
        spec.Format = ImageFormat::RGBA32F;
        spec.GenerateMips = true;
        spec.MipLevels = kMipLevels;
        Ref<TextureCubemap> src = TextureCubemap::Create(spec);
        ASSERT_TRUE(src != nullptr);

        auto MakePattern = [](u32 mip, u32 face, u32 x, u32 y) -> std::array<f32, 4>
        {
            const f32 base = static_cast<f32>(mip * 100 + face * 10) / 1000.0f;
            return {
                base + 0.0001f * static_cast<f32>(x),
                base + 0.0001f * static_cast<f32>(y),
                base + 0.5f,
                1.0f,
            };
        };

        for (u32 mip = 0; mip < kMipLevels; ++mip)
        {
            const u32 mipW = std::max(1u, kWidth >> mip);
            const u32 mipH = std::max(1u, kHeight >> mip);
            std::vector<f32> buf(static_cast<std::size_t>(mipW) * mipH * 4);
            for (u32 face = 0; face < 6; ++face)
            {
                for (u32 y = 0; y < mipH; ++y)
                {
                    for (u32 x = 0; x < mipW; ++x)
                    {
                        auto texel = MakePattern(mip, face, x, y);
                        const std::size_t idx = (static_cast<std::size_t>(y) * mipW + x) * 4;
                        buf[idx + 0] = texel[0];
                        buf[idx + 1] = texel[1];
                        buf[idx + 2] = texel[2];
                        buf[idx + 3] = texel[3];
                    }
                }
                const u32 byteSize = static_cast<u32>(buf.size() * sizeof(f32));
                ASSERT_TRUE(src->SetFaceDataMip(face, mip, buf.data(), byteSize));
            }
        }

        // -- Minimal stand-in irradiance cubemap (1 mip, uniform gray).
        CubemapSpecification irradianceSpec{};
        irradianceSpec.Width = 16;
        irradianceSpec.Height = 16;
        irradianceSpec.Format = ImageFormat::RGBA32F;
        irradianceSpec.GenerateMips = false;
        irradianceSpec.MipLevels = 1;
        Ref<TextureCubemap> irradiance = TextureCubemap::Create(irradianceSpec);
        ASSERT_TRUE(irradiance != nullptr);
        {
            std::vector<f32> gray(16u * 16u * 4u, 0.5f);
            for (u32 face = 0; face < 6; ++face)
                ASSERT_TRUE(irradiance->SetFaceDataMip(face, 0,
                                                       gray.data(), static_cast<u32>(gray.size() * sizeof(f32))));
        }

        // -- Minimal stand-in BRDF LUT (8x8, RGBA32F).
        TextureSpecification brdfSpec{};
        brdfSpec.Width = 8;
        brdfSpec.Height = 8;
        brdfSpec.Format = ImageFormat::RGBA32F;
        brdfSpec.GenerateMips = false;
        Ref<Texture2D> brdf = Texture2D::Create(brdfSpec);
        ASSERT_TRUE(brdf != nullptr);
        {
            std::vector<f32> brdfPixels(8u * 8u * 4u);
            for (std::size_t i = 0; i < brdfPixels.size(); ++i)
                brdfPixels[i] = static_cast<f32>(i) / static_cast<f32>(brdfPixels.size());
            brdf->SetData(brdfPixels.data(), static_cast<u32>(brdfPixels.size() * sizeof(f32)));
        }

        // -- Save.
        IBLConfiguration config{};
        const std::string sourcePath = "test_ibl_cache_roundtrip.hdr";
        ASSERT_TRUE(IBLCache::Save(sourcePath, config, irradiance, src, brdf));

        // -- Load.
        IBLCache::CachedIBL cached;
        ASSERT_TRUE(IBLCache::TryLoad(sourcePath, config, cached));
        ASSERT_TRUE(cached.IsValid());
        ASSERT_TRUE(cached.Prefilter != nullptr);
        EXPECT_EQ(cached.Prefilter->GetMipLevelCount(), kMipLevels)
            << "Loaded prefilter lost its mip chain";

        // -- Verify every mip of every face bit-exact.
        u32 totalDiffs = 0;
        for (u32 mip = 0; mip < kMipLevels; ++mip)
        {
            const u32 mipW = std::max(1u, kWidth >> mip);
            const u32 mipH = std::max(1u, kHeight >> mip);
            for (u32 face = 0; face < 6; ++face)
            {
                std::vector<u8> readBytes;
                ASSERT_TRUE(cached.Prefilter->GetFaceData(face, readBytes, mip));
                ASSERT_EQ(readBytes.size(), static_cast<std::size_t>(mipW) * mipH * 4 * sizeof(f32));

                const f32* readFloats = reinterpret_cast<const f32*>(readBytes.data());
                for (u32 y = 0; y < mipH; ++y)
                {
                    for (u32 x = 0; x < mipW; ++x)
                    {
                        auto expected = MakePattern(mip, face, x, y);
                        const std::size_t idx = (static_cast<std::size_t>(y) * mipW + x) * 4;
                        for (u32 c = 0; c < 4; ++c)
                        {
                            if (std::memcmp(&expected[c], &readFloats[idx + c], sizeof(f32)) != 0)
                                ++totalDiffs;
                        }
                    }
                }
            }
        }
        EXPECT_EQ(totalDiffs, 0u)
            << "IBL cache round-trip produced " << totalDiffs
            << " bit-different texels across all mips (classic 'only mip 0 loaded' symptom)";

        IBLCache::Shutdown();
        std::filesystem::remove_all(tempDir, ec);
    }
} // namespace OloEngine::Tests
