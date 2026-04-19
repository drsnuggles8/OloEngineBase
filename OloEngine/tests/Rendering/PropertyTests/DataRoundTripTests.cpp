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

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
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
} // namespace OloEngine::Tests
