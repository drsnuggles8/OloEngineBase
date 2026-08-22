#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// FacadeReadbackParityTest — GPU contract test (skips cleanly without GL 4.5+).
//
// #810 moved olo_render_probe_pixel and olo_render_target_stats off raw
// glGetTextureSubImage onto RenderCommand::ReadTextureSubImage, the facade
// readback spine, so they answer on Vulkan too. The risk that change carries is
// not "does it compile on Vulkan" — it is that the OpenGL arm quietly starts
// returning DIFFERENT numbers, which no screenshot and no green pass-suite
// would show, and which every later investigation would then trust.
//
// So this pins the thing a cross-backend A/B cannot: on ONE backend, in ONE
// process, the facade read and the raw-GL read of the same texels must be
// BIT-IDENTICAL. A cross-build comparison could not do this — it would be
// comparing two binaries against two frames.
//
// It also pins RendererAPI::QueryTextureFormat, the neutral format description
// the tools now branch on — a wrong channel count there sizes every readback
// wrong, and IsDepth/IsInteger pick the destination format:
//   * a DEPTH source must be read with a depth destination, because only those
//     lower to GL_DEPTH_COMPONENT (reading depth as GL_RED is
//     GL_INVALID_OPERATION, i.e. a silently zero-filled buffer);
//   * an INTEGER source must stay integer, so the R32I entity-id attachment
//     comes back as the exact id rather than a float.
// Depth itself is exercised live rather than here: Texture2D::Create has no
// depth format, so the depth destination is covered by the render-graph
// captures in the PR's cross-backend A/B, not by a synthetic texture.
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Texture.h"

#include <glad/gl.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

// OLO_TEST_LAYER: L1

namespace
{
    using namespace OloEngine;

    constexpr u32 kWidth = 16;
    constexpr u32 kHeight = 12;

    // A texture created through the engine, so it carries an RHI identity —
    // which is exactly what the facade path needs and a bare glCreateTextures
    // name does not have.
    [[nodiscard]] Ref<Texture2D> CreateIdentityTexture(ImageFormat format)
    {
        TextureSpecification spec;
        spec.Width = kWidth;
        spec.Height = kHeight;
        spec.Format = format;
        spec.GenerateMips = false;
        return Texture2D::Create(spec);
    }
} // namespace

// The core parity claim: same texture, same rect, two read paths, identical
// bytes. RGBA8 because it is the format most of the render graph uses and the
// one where a channel-count mistake is least visible.
TEST(FacadeReadbackParity, FacadeAndRawGLAgreeBitExactlyOnRGBA8)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<Texture2D> texture = CreateIdentityTexture(ImageFormat::RGBA8);
    ASSERT_TRUE(texture);
    const RHI::ResourceHandle handle = texture->GetRHIHandle();
    ASSERT_TRUE(handle.IsValid());
    const u32 nativeId = texture->GetRendererID();
    ASSERT_NE(nativeId, 0u);

    // Upload a deterministic pattern through the engine so both reads see the
    // same storage.
    std::vector<u8> upload(static_cast<sizet>(kWidth) * kHeight * 4);
    for (sizet i = 0; i < upload.size(); ++i)
        upload[i] = static_cast<u8>((i * 7u + 13u) & 0xFFu);
    texture->SetData(upload.data(), static_cast<u32>(upload.size()));

    const sizet valueCount = static_cast<sizet>(kWidth) * kHeight * 4;

    std::vector<f32> viaFacade(valueCount, -1.0f);
    ASSERT_TRUE(RenderCommand::ReadTextureSubImage(handle, 0, 0, 0, 0, kWidth, kHeight, 1,
                                                   RHI::Format::RGBA32Float,
                                                   viaFacade.size() * sizeof(f32), viaFacade.data()));

    std::vector<f32> viaRawGL(valueCount, -2.0f);
    while (glGetError() != GL_NO_ERROR)
    {
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureSubImage(nativeId, 0, 0, 0, 0, static_cast<GLsizei>(kWidth), static_cast<GLsizei>(kHeight), 1,
                         GL_RGBA, GL_FLOAT, static_cast<GLsizei>(viaRawGL.size() * sizeof(f32)), viaRawGL.data());
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    // Bit-exact, not approximately equal: both paths ask the driver for the
    // same conversion, so any difference is a bug in the plumbing rather than
    // floating-point slack.
    EXPECT_EQ(std::memcmp(viaFacade.data(), viaRawGL.data(), valueCount * sizeof(f32)), 0)
        << "the facade readback and the raw GL readback disagree — every diagnostic built on the facade "
           "path is now reporting different numbers than the GL arm did";
}

// A single texel, the shape olo_render_probe_pixel actually uses. A whole-image
// read agreeing does not prove a 1x1 read at an offset agrees.
TEST(FacadeReadbackParity, SingleTexelReadAgreesWithRawGLAtAnOffset)
{
    OLO_ENSURE_GPU_OR_SKIP();

    Ref<Texture2D> texture = CreateIdentityTexture(ImageFormat::RGBA8);
    ASSERT_TRUE(texture);
    const RHI::ResourceHandle handle = texture->GetRHIHandle();
    ASSERT_TRUE(handle.IsValid());

    std::vector<u8> upload(static_cast<sizet>(kWidth) * kHeight * 4);
    for (sizet i = 0; i < upload.size(); ++i)
        upload[i] = static_cast<u8>((i * 11u + 5u) & 0xFFu);
    texture->SetData(upload.data(), static_cast<u32>(upload.size()));

    constexpr i32 kX = 5;
    constexpr i32 kY = 3;

    std::array<f32, 4> viaFacade{ -1.0f, -1.0f, -1.0f, -1.0f };
    ASSERT_TRUE(RenderCommand::ReadTextureSubImage(handle, 0, kX, kY, 0, 1, 1, 1, RHI::Format::RGBA32Float,
                                                   viaFacade.size() * sizeof(f32), viaFacade.data()));

    std::array<f32, 4> viaRawGL{ -2.0f, -2.0f, -2.0f, -2.0f };
    while (glGetError() != GL_NO_ERROR)
    {
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureSubImage(texture->GetRendererID(), 0, kX, kY, 0, 1, 1, 1, GL_RGBA, GL_FLOAT,
                         static_cast<GLsizei>(viaRawGL.size() * sizeof(f32)), viaRawGL.data());
    ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    EXPECT_EQ(std::memcmp(viaFacade.data(), viaRawGL.data(), viaFacade.size() * sizeof(f32)), 0);
}

// QueryTextureFormat is what the tools branch on: channel count sizes the
// readback buffer, IsDepth/IsInteger pick the destination format. A wrong
// answer here is a wrong read everywhere downstream.
TEST(FacadeReadbackParity, QueryTextureFormatDescribesEngineTextures)
{
    OLO_ENSURE_GPU_OR_SKIP();

    struct Case
    {
        ImageFormat Format;
        const char* Token;
        u8 Channels;
        bool IsDepth;
        bool IsInteger;
        bool IsFloat;
    };
    // Only formats Texture2D::Create actually supports; each is a live render
    // graph format, so the table is not hypothetical.
    const std::array<Case, 3> cases{ {
        { ImageFormat::RGBA8, "RGBA8", 4, false, false, false },
        { ImageFormat::RGBA16F, "RGBA16F", 4, false, false, true },
        { ImageFormat::R32I, "R32I", 1, false, true, false },
    } };

    for (const Case& c : cases)
    {
        const Ref<Texture2D> texture = CreateIdentityTexture(c.Format);
        ASSERT_TRUE(texture) << c.Token;

        RHI::TextureFormatInfo info;
        ASSERT_TRUE(RenderCommand::QueryTextureFormat(texture->GetRHIHandle(), 0, info)) << c.Token;
        EXPECT_STREQ(info.Token, c.Token);
        EXPECT_EQ(info.Channels, c.Channels) << c.Token;
        EXPECT_EQ(info.IsDepth, c.IsDepth) << c.Token;
        EXPECT_EQ(info.IsInteger, c.IsInteger) << c.Token;
        EXPECT_EQ(info.IsFloat, c.IsFloat) << c.Token;
        EXPECT_NE(info.Native, 0u) << c.Token << ": the native format enum must travel too (amendment (77))";
    }
}

// A mip that does not exist must be REFUSED, not answered. The old GL arm
// checked glGetTextureLevelParameteriv's width for this; the replacement has to
// keep that behaviour or a probe of a missing mip reads whatever the driver
// leaves in the buffer.
TEST(FacadeReadbackParity, QueryTextureFormatRefusesAMipWithNoStorage)
{
    OLO_ENSURE_GPU_OR_SKIP();

    const Ref<Texture2D> texture = CreateIdentityTexture(ImageFormat::RGBA8);
    ASSERT_TRUE(texture);

    RHI::TextureFormatInfo info;
    EXPECT_FALSE(RenderCommand::QueryTextureFormat(texture->GetRHIHandle(), 9, info))
        << "a mip level past the chain must refuse rather than describe mip 0";
}

// A stale identity must resolve to nothing, on the format query as well as on
// the read. This is the failure the handle exists to make impossible: a
// destroyed texture's native name can be reissued, and a diagnostic that
// answered from it would be confidently describing a different object.
TEST(FacadeReadbackParity, StaleHandleIsRefusedRatherThanAnswered)
{
    OLO_ENSURE_GPU_OR_SKIP();

    RHI::ResourceHandle stale;
    {
        const Ref<Texture2D> texture = CreateIdentityTexture(ImageFormat::RGBA8);
        ASSERT_TRUE(texture);
        stale = texture->GetRHIHandle();
        ASSERT_TRUE(stale.IsValid());
    }

    RHI::TextureFormatInfo info;
    EXPECT_FALSE(RenderCommand::QueryTextureFormat(stale, 0, info));

    std::array<f32, 4> destination{ 0.0f, 0.0f, 0.0f, 0.0f };
    EXPECT_FALSE(RenderCommand::ReadTextureSubImage(stale, 0, 0, 0, 0, 1, 1, 1, RHI::Format::RGBA32Float,
                                                    destination.size() * sizeof(f32), destination.data()));
}

// A DEPTH source read with a COLOUR destination format is the regression this
// pins, and it is a nasty one: on GL only depth destinations lower to
// GL_DEPTH_COMPONENT, and asking for GL_RED on a depth texture is
// GL_INVALID_OPERATION. Before #810 folded the capture fork, the facade's
// depth path was Vulkan-only and named R32Float; the fold routed GL through it
// too, which broke every depth capture until this test's subject was fixed.
//
// It is asserted through a framebuffer's depth attachment because
// Texture2D::Create has no depth format — that is also the only way the render
// graph's depth targets exist, so this is the production shape.
TEST(FacadeReadbackParity, DepthAttachmentReadsThroughADepthDestinationFormat)
{
    OLO_ENSURE_GPU_OR_SKIP();

    FramebufferSpecification spec;
    spec.Width = kWidth;
    spec.Height = kHeight;
    spec.Attachments = FramebufferAttachmentSpecification{ { FramebufferTextureFormat::RGBA8 },
                                                           { FramebufferTextureFormat::DEPTH24STENCIL8 } };
    const Ref<Framebuffer> framebuffer = Framebuffer::Create(spec);
    ASSERT_TRUE(framebuffer);

    const RHI::ResourceHandle depth = framebuffer->GetDepthAttachmentHandle();
    ASSERT_TRUE(depth.IsValid()) << "the fixture needs a real depth attachment to say anything";

    RHI::TextureFormatInfo info;
    ASSERT_TRUE(RenderCommand::QueryTextureFormat(depth, 0, info));
    EXPECT_TRUE(info.IsDepth) << "a depth attachment must describe itself as depth, or every caller below "
                                 "picks a colour destination for it";
    EXPECT_EQ(info.Channels, 1);

    // The destination the tools actually choose for a depth source.
    std::vector<f32> depths(static_cast<sizet>(kWidth) * kHeight, -1.0f);
    EXPECT_TRUE(RenderCommand::ReadTextureSubImage(depth, 0, 0, 0, 0, kWidth, kHeight, 1, RHI::Format::D32Float,
                                                   depths.size() * sizeof(f32), depths.data()))
        << "a depth readback with a depth destination must succeed on every backend";

    // Every value must be a real depth. The buffer starts at -1.0f, so an
    // untouched destination — the shape a silently-failed read leaves behind —
    // is caught here rather than reported as "the depth buffer is negative".
    for (const f32 d : depths)
    {
        EXPECT_TRUE(std::isfinite(d)) << "non-finite depth read back";
        EXPECT_GE(d, 0.0f) << "the destination was not written — the readback silently did nothing";
        EXPECT_LE(d, 1.0f);
    }
}
