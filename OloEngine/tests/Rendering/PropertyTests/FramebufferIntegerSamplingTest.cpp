// OLO_TEST_LAYER: L2
#include "OloEnginePCH.h"
#include "RenderPropertyTest.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <vector>

namespace OloEngine::Tests
{
    TEST(FramebufferIntegerSampling, StorageIsCompleteForIntegerTexelFetchAfterCreateAndResize)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard guard("FramebufferIntegerSampling", GLStateGuard::Policy::Restore);
        FramebufferSpecification spec;
        spec.Width = spec.Height = 32;
        spec.Attachments = { FramebufferTextureFormat::RED_INTEGER };
        auto ids = Framebuffer::Create(spec);
        ASSERT_TRUE(ids);
        spec.Attachments = { FramebufferTextureFormat::RGBA32F };
        auto output = Framebuffer::Create(spec);
        ASSERT_TRUE(output);
        auto shader = Shader::Create("assets/shaders/tests/FramebufferIntegerProbe.glsl");
        ASSERT_TRUE(shader);
        for (const u32 size : { 32u, 64u })
        {
            ids->Resize(size, size);
            output->Resize(size, size);
            ids->ClearAttachment(0, 37);
            const auto texture = ids->GetColorAttachmentRendererID(0);
            for (const GLenum filter : { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER })
            {
                GLint value = 0;
                glGetTextureParameteriv(texture, filter, &value);
                EXPECT_EQ(value, GL_NEAREST);
            }
            output->Bind();
            glViewport(0, 0, size, size);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            shader->Bind();
            glBindTextureUnit(12, texture);
            FullscreenPass triangle;
            triangle.Draw(0);
            glFinish();
            output->Unbind();
            std::vector<f32> pixels;
            ReadbackRgbaFloat(output->GetColorAttachmentHandle(0), size, size, pixels);
            ASSERT_EQ(pixels.size(), size * size * 4u);
            for (sizet i = 0; i < pixels.size(); i += 4u)
                ASSERT_FLOAT_EQ(pixels[i], 37.0f);
            glBindTextureUnit(12, 0);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }
    }
} // namespace OloEngine::Tests
