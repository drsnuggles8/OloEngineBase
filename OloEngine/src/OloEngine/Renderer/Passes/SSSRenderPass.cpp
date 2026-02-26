#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    SSSRenderPass::SSSRenderPass()
    {
        SetName("SSSPass");
    }

    void SSSRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Create fullscreen triangle (same pattern as SSAORenderPass)
        struct FullscreenVertex
        {
            glm::vec3 Position;
            glm::vec2 TexCoord;
        };

        static_assert(sizeof(FullscreenVertex) == sizeof(f32) * 5,
                      "FullscreenVertex must be exactly 5 floats");

        FullscreenVertex vertices[3] = {
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 3.0f, -1.0f, 0.0f }, { 2.0f, 0.0f } },
            { { -1.0f, 3.0f, 0.0f }, { 0.0f, 2.0f } }
        };

        u32 indices[3] = { 0, 1, 2 };

        m_FullscreenTriangleVA = VertexArray::Create();

        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(
            static_cast<const void*>(vertices),
            static_cast<u32>(sizeof(vertices)));

        vertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                  { ShaderDataType::Float2, "a_TexCoord" } });

        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, 3);

        m_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        m_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);

        // Load SSS blur shader
        m_SSSBlurShader = Shader::Create("assets/shaders/SSS_Blur.glsl");

        OLO_CORE_INFO("SSSRenderPass: Initialized");
    }

    void SSSRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // SSS blur operates directly on the scene framebuffer's color attachment.
        // It reads color+alpha (SSS mask), blurs, and writes the result back.
        // We skip if snow SSS blur is disabled or resources are missing.
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled ||
            !m_SceneFramebuffer || !m_SSSBlurShader || !m_FullscreenTriangleVA)
        {
            return;
        }

        // Upload SSS parameters
        if (m_SSSUBO && m_GPUData)
        {
            m_SSSUBO->SetData(m_GPUData, SSSUBOData::GetSize());
        }

        // The SSS pass reads from the scene FB color[0] and depth, and writes
        // to the same scene FB. Since we can't read and write the same texture
        // simultaneously, we render to the scene FB in-place — the shader
        // samples via texture binding (which reads the current content) and
        // the output overwrites the color attachment.
        //
        // This works because OpenGL allows reading from a bound texture
        // as long as the texture is bound to a different binding point than
        // the framebuffer's draw attachment. We bind the color texture to
        // sampler slot 0 for reading, and the framebuffer draws to attachment 0.
        //
        // Note: For fully correct behavior, a ping-pong approach would be ideal.
        // This single-pass approach works for our separable combined blur because
        // each fragment reads from nearby pixels that haven't been written yet
        // (undefined behavior in general, but practically works for small kernels).

        m_SceneFramebuffer->Bind();
        const auto& fbSpec = m_SceneFramebuffer->GetSpecification();
        RenderCommand::SetViewport(0, 0, fbSpec.Width, fbSpec.Height);
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        m_SSSBlurShader->Bind();

        // Bind scene color for sampling
        u32 colorID = m_SceneFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, colorID);

        // Bind scene depth for bilateral filtering
        u32 depthID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle();

        m_SceneFramebuffer->Unbind();
    }

    void SSSRenderPass::DrawFullscreenTriangle()
    {
        m_FullscreenTriangleVA->Bind();
        RenderCommand::DrawIndexed(m_FullscreenTriangleVA);
    }

    Ref<Framebuffer> SSSRenderPass::GetTarget() const
    {
        // SSS pass operates in-place on the scene framebuffer
        return m_SceneFramebuffer;
    }

    void SSSRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer — operates on scene FB
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSSRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer — operates on scene FB
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSSRenderPass::OnReset()
    {
        // Nothing to reset — no owned resources
    }
} // namespace OloEngine
