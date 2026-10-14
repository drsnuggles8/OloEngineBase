// OLO_TEST_LAYER: L4
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include <gtest/gtest.h>
#include <glad/gl.h>
#include <array>

namespace OloEngine::Tests
{
    class GBufferDepthEntityResolveTest : public RendererAttachedTest
    {
        void BuildScene() override {}
    };

    TEST_F(GBufferDepthEntityResolveTest, MixedMsaaDepthAndEntityOwnershipComeFromTheSameSample)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        GLStateGuard guard("GBufferDepthOwnershipTest", GLStateGuard::Policy::Restore);
        ASSERT_GE(RenderCommand::GetMaxFramebufferSamples(), 4u);
        auto gbuffer = GBuffer::Create(16, 16, 4);
        ASSERT_TRUE(gbuffer);
        auto source = gbuffer->GetFramebuffer();
        auto shader = Shader::Create("assets/shaders/tests/GBufferDepthEntityProbe.glsl");
        ASSERT_TRUE(source && shader);
        source->Bind();
        RenderCommand::SetViewport(0, 0, 16, 16);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Always);
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        shader->Bind();
        auto triangle = MeshPrimitives::GetFullscreenTriangle();
        triangle->Bind();
        RenderCommand::DrawIndexed(triangle);
        source->Unbind();
        gbuffer->Resolve(); // the production owner, including both shader fixups
        glFinish();
        std::array<f32, 16u * 16u> depth{};
        glGetTextureImage(gbuffer->GetDepthAttachmentID(), 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                          static_cast<GLsizei>(sizeof(depth)), depth.data());
        auto resolved = gbuffer->GetSamplingFramebuffer();
        for (u32 y = 0; y < 16u; ++y)
            for (u32 x = 0; x < 16u; ++x)
            {
                // Sample zero carries body 17 at .75. All other samples
                // carry prop 29 at .15: an independent/minimum depth resolve
                // paired with sample-zero ID is an observable wrong answer.
                EXPECT_NEAR(depth[y * 16u + x], 0.75f, 0.000001f);
                EXPECT_EQ(resolved->ReadPixel(std::to_underlying(GBuffer::EntityID), x, y), 17);
            }
        const auto ids = gbuffer->GetColorAttachmentID(GBuffer::EntityID);
        for (const GLenum filter : { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER })
        {
            GLint value = 0;
            glGetTextureParameteriv(ids, filter, &value);
            EXPECT_EQ(value, GL_NEAREST);
        }
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
        CommandDispatch::InvalidateBindingCaches();
    }
} // namespace OloEngine::Tests
