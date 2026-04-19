// =============================================================================
// RendererValidateTest.cpp
//
// Tests for the Layer-7 (smoke / sanity readback) helper. Verifies that
// ValidateFramebuffer correctly identifies NaN, Inf, and fp16-overflow
// conditions in float framebuffers, and that clean outputs pass.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Debug/RendererValidate.h"
#include "OloEngine/Renderer/Framebuffer.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Build a small RGBA16F framebuffer pre-populated with a given float
        // buffer. Caller supplies `width * height * 4` floats.
        Ref<Framebuffer> MakeFloatFbFilled(u32 width, u32 height, const f32* pixels)
        {
            FramebufferSpecification spec{};
            spec.Width = width;
            spec.Height = height;
            spec.Attachments = { FramebufferTextureFormat::RGBA32F };
            auto fb = Framebuffer::Create(spec);
            const GLuint tex = static_cast<GLuint>(fb->GetColorAttachmentRendererID(0));
            ::glTextureSubImage2D(tex, 0, 0, 0,
                static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                GL_RGBA, GL_FLOAT, pixels);
            return fb;
        }
    } // namespace

    TEST(RendererValidateTest, CleanFramebufferPassesValidation)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 8, H = 8;
        std::vector<f32> pixels(W * H * 4, 0.5f);
        auto fb = MakeFloatFbFilled(W, H, pixels.data());

        auto stats = RendererValidate::ReadFloatAttachmentStats(fb, 0);
        EXPECT_EQ(stats.m_NanCount, 0u);
        EXPECT_EQ(stats.m_InfCount, 0u);
        EXPECT_EQ(stats.m_PixelCount, W * H);
        EXPECT_NEAR(stats.m_AvgR, 0.5, 1e-5);

        EXPECT_TRUE(RendererValidate::ValidateFramebuffer(fb, "CleanTest", 0, /*assertOnFailure=*/false));
    }

    TEST(RendererValidateTest, NanPixelsAreDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 4, H = 4;
        std::vector<f32> pixels(W * H * 4, 1.0f);
        // Splat one NaN into the green channel of pixel (0,0).
        pixels[1] = std::numeric_limits<f32>::quiet_NaN();
        auto fb = MakeFloatFbFilled(W, H, pixels.data());

        auto stats = RendererValidate::ReadFloatAttachmentStats(fb, 0);
        EXPECT_GE(stats.m_NanCount, 1u);

        // Clean attachment + NaN → ValidateFramebuffer returns false (in Debug).
#ifdef OLO_DEBUG
        EXPECT_FALSE(RendererValidate::ValidateFramebuffer(fb, "NanTest", 0, /*assertOnFailure=*/false));
#endif
    }

    TEST(RendererValidateTest, InfPixelsAreDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 4, H = 4;
        std::vector<f32> pixels(W * H * 4, 1.0f);
        pixels[2] = std::numeric_limits<f32>::infinity();
        auto fb = MakeFloatFbFilled(W, H, pixels.data());

        auto stats = RendererValidate::ReadFloatAttachmentStats(fb, 0);
        EXPECT_GE(stats.m_InfCount, 1u);

#ifdef OLO_DEBUG
        EXPECT_FALSE(RendererValidate::ValidateFramebuffer(fb, "InfTest", 0, /*assertOnFailure=*/false));
#endif
    }

    TEST(RendererValidateTest, Fp16OverflowIsDetected)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr u32 W = 4, H = 4;
        std::vector<f32> pixels(W * H * 4, 0.0f);
        pixels[0] = 70000.0f; // exceeds fp16 max of 65504
        auto fb = MakeFloatFbFilled(W, H, pixels.data());

#ifdef OLO_DEBUG
        EXPECT_FALSE(RendererValidate::ValidateFramebuffer(fb, "OverflowTest", 0, /*assertOnFailure=*/false));
#endif
    }

    TEST(RendererValidateTest, RejectsUnsupportedFormatsGracefully)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        FramebufferSpecification spec{};
        spec.Width = 4;
        spec.Height = 4;
        spec.Attachments = { FramebufferTextureFormat::RGBA8 }; // not a float format
        auto fb = Framebuffer::Create(spec);

        auto stats = RendererValidate::ReadFloatAttachmentStats(fb, 0);
        EXPECT_EQ(stats.m_PixelCount, 0u) << "unsupported format should return zero-stats";

        // ValidateFramebuffer returns true on unsupported formats (no opinion).
        EXPECT_TRUE(RendererValidate::ValidateFramebuffer(fb, "RGBA8Test", 0, /*assertOnFailure=*/false));
    }
} // namespace OloEngine::Tests
