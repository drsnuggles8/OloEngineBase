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
#include <limits>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace OloEngine::Tests
{
    namespace
    {
        // RAII: initialise IBLCache against a temp directory and tear it all
        // down on scope exit, so ASSERT_* early-exits can't leak the shared
        // cache state across tests or leave stray files on disk.
        struct ScopedIBLCacheGuard
        {
            std::filesystem::path m_TempDir;

            explicit ScopedIBLCacheGuard(std::filesystem::path tempDir)
                : m_TempDir(std::move(tempDir))
            {
                std::error_code ec;
                std::filesystem::remove_all(m_TempDir, ec);
                IBLCache::Initialize(m_TempDir);
            }

            ~ScopedIBLCacheGuard()
            {
                IBLCache::Shutdown();
                std::error_code ec;
                std::filesystem::remove_all(m_TempDir, ec);
            }

            ScopedIBLCacheGuard(const ScopedIBLCacheGuard&) = delete;
            ScopedIBLCacheGuard& operator=(const ScopedIBLCacheGuard&) = delete;
        };
    } // namespace

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

        // Temp cache directory so we don't stomp on the real one.
        // ScopedIBLCacheGuard handles Initialize/Shutdown + remove_all
        // symmetrically across all exit paths, including ASSERT_* aborts.
        // PID + random suffix isolates parallel test runners (ctest -j N)
        // AND defeats predictable-path attacks on world-writable tmp dirs
        // (SonarQube cpp:S5443 — don't trust publicly-writeable locations).
        std::random_device rd;
        const auto randomTag = std::to_string(rd()) + "_" + std::to_string(rd());
        auto tempDir = std::filesystem::temp_directory_path() /
                       (std::string("olo_ibl_cache_test_") +
                        std::to_string(
#if defined(_WIN32)
                            static_cast<unsigned long>(::_getpid())
#else
                            static_cast<unsigned long>(::getpid())
#endif
                                ) +
                        "_" + randomTag);
        // Fail fast if the path already exists (symlink-follow attack
        // mitigation) — ScopedIBLCacheGuard will create it freshly.
        std::error_code probeEc;
        ASSERT_FALSE(std::filesystem::exists(tempDir, probeEc))
            << "temp cache path unexpectedly exists: " << tempDir.string();
        ScopedIBLCacheGuard cacheGuard(tempDir);

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

                // Avoid reinterpret_cast-through-std::vector<u8>: copy each
                // 4-byte texel into a local f32 via std::memcpy so we only
                // touch the underlying bytes through their canonical type.
                // Strict-aliasing safe and matches the pattern used in the
                // RGBA32F bit-identity test above.
                for (u32 y = 0; y < mipH; ++y)
                {
                    for (u32 x = 0; x < mipW; ++x)
                    {
                        auto expected = MakePattern(mip, face, x, y);
                        const std::size_t idx = (static_cast<std::size_t>(y) * mipW + x) * 4;
                        for (u32 c = 0; c < 4; ++c)
                        {
                            f32 readValue = 0.0f;
                            std::memcpy(&readValue,
                                        readBytes.data() + (idx + c) * sizeof(f32),
                                        sizeof(f32));
                            if (std::memcmp(&expected[c], &readValue, sizeof(f32)) != 0)
                                ++totalDiffs;
                        }
                    }
                }
            }
        }
        EXPECT_EQ(totalDiffs, 0u)
            << "IBL cache round-trip produced " << totalDiffs
            << " bit-different texels across all mips (classic 'only mip 0 loaded' symptom)";

        // cacheGuard destructor runs IBLCache::Shutdown + remove_all(tempDir).
    }

    // =========================================================================
    // Layer-11 fuzz surrogate: randomised stress over the RGBA32F GPU
    // upload/readback path. Sweeps many (width, height, seed) tuples with a
    // mix of interesting IEEE-754 value classes (zero, ±denormal, ±normal,
    // ±large, signed zero, pow-of-two boundaries). Asserts bit-identity per
    // iteration. Serves as a minimum-viable fuzzing proxy without pulling in
    // libFuzzer — when a driver update or upload path regresses on a specific
    // dimension or value class, this catches it without needing a curated
    // corpus.
    //
    // Deterministic: seed is the iteration index, so failures reproduce
    // on replay.
    // =========================================================================
    TEST(DataRoundTripTest, RandomisedRgba32FStressRoundTrip)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Interesting IEEE-754 value classes we want the upload path to
        // survive bit-exact. Keeps NaN/Inf out: those don't round-trip
        // via memcmp because bit patterns are non-unique.
        static constexpr f32 kSamples[] = {
            0.0f,
            -0.0f,
            std::numeric_limits<f32>::min(), // smallest normal
            -std::numeric_limits<f32>::min(),
            std::numeric_limits<f32>::denorm_min(), // smallest denormal
            -std::numeric_limits<f32>::denorm_min(),
            1.0f,
            -1.0f,
            2.0f,
            -2.0f,
            0.5f,
            -0.5f,
            std::numeric_limits<f32>::max(),
            -std::numeric_limits<f32>::max(),
            3.14159265f,
            -3.14159265f,
            1.0e-20f,
            1.0e+20f,
            65504.0f, // fp16-max boundary
            0.99999994f,
            1.00000012f, // epsilon-boundary around 1.0
        };
        constexpr std::size_t kSampleCount = sizeof(kSamples) / sizeof(kSamples[0]);

        constexpr u32 kIterations = 32;
        u32 totalTexels = 0;

        for (u32 iter = 0; iter < kIterations; ++iter)
        {
            // Deterministic pseudo-random dimensions per iteration. Stick to
            // moderate sizes so the full sweep runs in well under a second.
            const u32 W = 1u + ((iter * 1103515245u + 12345u) >> 24) % 64u; // [1, 64]
            const u32 H = 1u + ((iter * 214013u + 2531011u) >> 24) % 64u;
            const u32 N = W * H * 4u;

            std::vector<f32> src(N);
            for (u32 i = 0; i < N; ++i)
            {
                // Mix the iteration into the index so different iterations
                // exercise different subsets of the value grid.
                const std::size_t sampleIdx = (i + iter * 7u) % kSampleCount;
                src[i] = kSamples[sampleIdx];
            }

            u32 tex = CreateFloatTexture2D(W, H, src.data());
            std::vector<f32> dst(N, 0.0f);
            ReadbackRgbaFloat(tex, W, H, dst);
            ::glDeleteTextures(1, reinterpret_cast<const GLuint*>(&tex));

            ASSERT_EQ(dst.size(), src.size());
            u32 diffs = 0;
            for (u32 i = 0; i < N; ++i)
            {
                if (std::memcmp(&src[i], &dst[i], sizeof(f32)) != 0)
                    ++diffs;
            }
            ASSERT_EQ(diffs, 0u)
                << "RGBA32F stress iter=" << iter
                << " W=" << W << " H=" << H
                << " produced " << diffs << " bit-different texels";
            totalTexels += N;
        }

        // Sanity: ensure we actually exercised a meaningful amount of data
        // across iterations (guards against a silent collapse to 1x1 tests).
        EXPECT_GT(totalTexels, kIterations * 4u * 4u);
    }

    // =========================================================================
    // Layer-11 fuzz surrogate for RGBA8: sweeps randomised dimensions and a
    // full 0..255 byte palette. Any upload/readback regression that corrupts
    // specific byte values (e.g. a sign-extension bug at 0x80) would surface
    // here.
    // =========================================================================
    TEST(DataRoundTripTest, RandomisedRgba8StressRoundTrip)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 kIterations = 32;
        u32 totalBytes = 0;

        for (u32 iter = 0; iter < kIterations; ++iter)
        {
            const u32 W = 1u + ((iter * 1103515245u + 12345u) >> 24) % 64u;
            const u32 H = 1u + ((iter * 214013u + 2531011u) >> 24) % 64u;
            const u32 N = W * H * 4u;

            std::vector<u8> src(N);
            // Linear congruential per-iteration so every iteration has a
            // different byte distribution. Seed with iter to be deterministic.
            u32 rng = iter * 2654435761u + 1u;
            for (u32 i = 0; i < N; ++i)
            {
                rng = rng * 1664525u + 1013904223u;
                src[i] = static_cast<u8>((rng >> 24) & 0xFFu);
            }

            GLuint tex = 0;
            ::glCreateTextures(GL_TEXTURE_2D, 1, &tex);
            ::glTextureStorage2D(tex, 1, GL_RGBA8,
                                 static_cast<GLsizei>(W), static_cast<GLsizei>(H));
            ::glTextureSubImage2D(tex, 0, 0, 0,
                                  static_cast<GLsizei>(W), static_cast<GLsizei>(H),
                                  GL_RGBA, GL_UNSIGNED_BYTE, src.data());

            std::vector<u8> dst(N, 0);
            ReadbackRgba8(static_cast<u32>(tex), W, H, dst);
            ::glDeleteTextures(1, &tex);

            u32 diffs = 0;
            for (u32 i = 0; i < N; ++i)
                if (src[i] != dst[i])
                    ++diffs;
            ASSERT_EQ(diffs, 0u)
                << "RGBA8 stress iter=" << iter
                << " W=" << W << " H=" << H
                << " produced " << diffs << " different bytes";
            totalBytes += N;
        }

        EXPECT_GT(totalBytes, kIterations * 4u * 4u);
    }
} // namespace OloEngine::Tests
