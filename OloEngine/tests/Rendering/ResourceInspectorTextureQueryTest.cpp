#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// ResourceInspectorTextureQueryTest — GPU contract test (skips cleanly without
// GL 4.5+).
//
// Pins the MIP-LEVEL COUNT the GL inspector backend reports for a texture, and
// the memory estimate derived from it.
//
// The count used to come from GL_TEXTURE_MAX_LEVEL, which is the sampling
// CEILING, not the number of levels that exist — and it defaults to 1000. So
// every texture in the engine reported 1001 levels and HasMips == true,
// including single-level render targets, and CalculateAccurateTextureMemoryUsage
// then summed one level per reported count. Nothing crashed and nothing looked
// wrong on screen; the inspector panel and the MCP resource tools simply
// answered a question incorrectly, which is the worst failure mode a diagnostic
// can have — the next investigation trusts the number.
//
// The three cases are the three ways a texture gets its storage, because the
// query differs per case: immutable storage answers exactly
// (GL_TEXTURE_IMMUTABLE_LEVELS), mutable storage has no such query and must be
// probed. The mutable single-level case is the one the old code got most
// wrong, since it is precisely where MAX_LEVEL is still sitting at its default.
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#include "Platform/OpenGL/OpenGLResourceInspectorBackend.h"

#include <glad/gl.h>

#include <algorithm>

// OLO_TEST_LAYER: L1

namespace
{
    using OloEngine::OpenGLResourceInspectorBackend;

    constexpr u32 kSize = 64; // 64 -> 32 -> ... -> 1 is a 7-level chain

    // Sum of w*h*4 over a full RGBA8 mip chain of `levels` levels from kSize.
    [[nodiscard]] sizet ExpectedRgba8Bytes(u32 levels)
    {
        sizet total = 0;
        u32 w = kSize;
        u32 h = kSize;
        for (u32 level = 0; level < levels; ++level)
        {
            total += static_cast<sizet>(w) * h * 4u;
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
        return total;
    }

    // Immutable storage with an exact level count.
    [[nodiscard]] u32 CreateImmutableRgba8(u32 levels)
    {
        u32 texture = 0;
        ::glCreateTextures(GL_TEXTURE_2D, 1, &texture);
        ::glTextureStorage2D(texture, static_cast<GLsizei>(levels), GL_RGBA8, kSize, kSize);
        return texture;
    }
} // namespace

TEST(ResourceInspectorTextureQuery, ImmutableSingleLevelTextureReportsOneMip)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const u32 texture = CreateImmutableRgba8(1);
    ASSERT_NE(texture, 0u);

    OpenGLResourceInspectorBackend backend;
    OpenGLResourceInspectorBackend::TextureQuery query;
    backend.QueryTexture(texture, /*isCubemap*/ false, query);

    EXPECT_EQ(query.Width, kSize);
    EXPECT_EQ(query.Height, kSize);
    EXPECT_EQ(query.MipLevels, 1u) << "a one-level texture must not report the MAX_LEVEL ceiling";
    EXPECT_FALSE(query.HasMips);
    EXPECT_EQ(query.MemoryUsage, ExpectedRgba8Bytes(1));

    ::glDeleteTextures(1, &texture);
}

TEST(ResourceInspectorTextureQuery, ImmutableMipChainReportsItsAllocatedLevelCount)
{
    OLO_ENSURE_GPU_OR_SKIP();

    constexpr u32 kLevels = 7;
    const u32 texture = CreateImmutableRgba8(kLevels);
    ASSERT_NE(texture, 0u);

    OpenGLResourceInspectorBackend backend;
    OpenGLResourceInspectorBackend::TextureQuery query;
    backend.QueryTexture(texture, /*isCubemap*/ false, query);

    EXPECT_EQ(query.MipLevels, kLevels);
    EXPECT_TRUE(query.HasMips);
    EXPECT_EQ(query.MemoryUsage, ExpectedRgba8Bytes(kLevels));

    ::glDeleteTextures(1, &texture);
}

TEST(ResourceInspectorTextureQuery, ImmutablePartialMipChainReportsOnlyTheLevelsItHas)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // Fewer levels than the extent could hold: the count must come from the
    // allocation, not from log2(size). A probe that walked to the theoretical
    // bottom would over-report here.
    constexpr u32 kLevels = 3;
    const u32 texture = CreateImmutableRgba8(kLevels);
    ASSERT_NE(texture, 0u);

    OpenGLResourceInspectorBackend backend;
    OpenGLResourceInspectorBackend::TextureQuery query;
    backend.QueryTexture(texture, /*isCubemap*/ false, query);

    EXPECT_EQ(query.MipLevels, kLevels);
    EXPECT_TRUE(query.HasMips);
    EXPECT_EQ(query.MemoryUsage, ExpectedRgba8Bytes(kLevels));

    ::glDeleteTextures(1, &texture);
}

TEST(ResourceInspectorTextureQuery, MutableSingleLevelTextureIsNotReportedAsAFullChain)
{
    OLO_ENSURE_GPU_OR_SKIP();

    // glTexImage2D on level 0 only, GL_TEXTURE_MAX_LEVEL left at its default of
    // 1000 — the exact shape that produced "1001 mip levels" and a memory
    // estimate inflated by the whole phantom chain.
    u32 texture = 0;
    ::glGenTextures(1, &texture);
    ASSERT_NE(texture, 0u);
    GLint previousBinding = 0;
    ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    ::glBindTexture(GL_TEXTURE_2D, texture);
    ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousBinding));

    GLint maxLevel = 0;
    ::glGetTextureParameteriv(texture, GL_TEXTURE_MAX_LEVEL, &maxLevel);
    ASSERT_GT(maxLevel, 0) << "precondition: MAX_LEVEL should still be at its permissive default";

    OpenGLResourceInspectorBackend backend;
    OpenGLResourceInspectorBackend::TextureQuery query;
    backend.QueryTexture(texture, /*isCubemap*/ false, query);

    EXPECT_EQ(query.MipLevels, 1u) << "only level 0 has storage, whatever MAX_LEVEL permits";
    EXPECT_FALSE(query.HasMips);
    EXPECT_EQ(query.MemoryUsage, ExpectedRgba8Bytes(1));

    ::glDeleteTextures(1, &texture);
}
