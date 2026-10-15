// OLO_TEST_LAYER: L2
#include "OloEnginePCH.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Groom/GroomSoftIntersection.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <vector>

namespace OloEngine::Tests
{
    TEST(GroomSoftIntersectionShader, CompiledCoverageAndProjectionMatchTheContracts)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard guard("GroomSoftIntersectionProbe", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = spec.Height = 64;
        spec.Attachments = { FramebufferTextureFormat::RGBA32F };
        auto target = Framebuffer::Create(spec);
        auto shader = Shader::Create("assets/shaders/tests/GroomSoftIntersectionProbe.glsl");
        ASSERT_TRUE(target && shader);
        target->Bind();
        glViewport(0, 0, 64, 64);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        shader->Bind();
        FullscreenPass pass;
        pass.Draw(0);
        glFinish();
        target->Unbind();
        std::vector<f32> pixels;
        ReadbackRgbaFloat(target->GetColorAttachmentRendererID(0), 64, 64, pixels);
        ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
        for (u32 y = 0; y < 64; ++y)
        {
            for (u32 x = 0; x < 64; ++x)
            {
                const sizet i = (y * 64u + x) * 4u;
                const f32 separation = (static_cast<f32>(x) - 8.0f) * 0.001f;
                const f32 t = static_cast<f32>(y) / 63.0f;
                EXPECT_NEAR(pixels[i], GroomIntersectionCoverage(separation, 0.01f, t, 0.25f), 0.00001f);
                EXPECT_NEAR(pixels[i + 1], GroomIntersectionCoverage(separation, 0.01f, t, 0.0f), 0.00001f);
                EXPECT_NEAR(pixels[i + 2], 3.0f, 0.0001f);
                EXPECT_NEAR(pixels[i + 3], 3.0f, 0.0001f);
            }
        }
    }
} // namespace OloEngine::Tests
