#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include <glad/gl.h>
#include <vector>

namespace OloEngine
{
    SSSRenderPass::SSSRenderPass()
    {
        SetName("SSSPass");
    }

    SSSRenderPass::~SSSRenderPass()
    {
        if (m_StagingTexture)
        {
            glDeleteTextures(1, &m_StagingTexture);
        }
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

        // Only run when snow is enabled AND SSS blur is explicitly turned on.
        // The alpha channel (SSS mask) is only consumed by this pass, so when
        // blur is off we simply leave the scene FB untouched — downstream passes
        // (PostProcess) write alpha=1.0 on all output paths, preventing leaks.
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled ||
            !m_SceneFramebuffer || !m_SSSBlurShader || !m_FullscreenTriangleVA)
        {
            return;
        }

        // SSS UBO is already uploaded by Renderer3D::EndScene each frame.

        const auto& fbSpec = m_SceneFramebuffer->GetSpecification();

        // Copy scene color to staging texture to avoid read-write hazard.
        u32 colorID = m_SceneFramebuffer->GetColorAttachmentRendererID(0);
        EnsureStagingTexture(fbSpec.Width, fbSpec.Height);
        glCopyImageSubData(
            colorID, GL_TEXTURE_2D, 0, 0, 0, 0,
            m_StagingTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
            static_cast<GLsizei>(fbSpec.Width),
            static_cast<GLsizei>(fbSpec.Height), 1);

        m_SceneFramebuffer->Bind();

        // Restrict drawing to color attachment 0 only — the scene FB has
        // multiple attachments (entity ID, normals) that must not be overwritten.
        GLenum drawBuf = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuf);

        RenderCommand::SetViewport(0, 0, fbSpec.Width, fbSpec.Height);
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        m_SSSBlurShader->Bind();

        // Bind staging copy for reading (not the live FB attachment)
        RenderCommand::BindTexture(0, m_StagingTexture);

        // Bind scene depth for bilateral filtering
        u32 depthID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle();

        // Restore all draw buffers for subsequent passes (derive count from framebuffer spec)
        const auto& attachments = fbSpec.Attachments.Attachments;
        std::vector<GLenum> allBufs;
        allBufs.reserve(attachments.size());
        u32 colorIndex = 0;
        for (const auto& att : attachments)
        {
            const auto fmt = att.TextureFormat;
            if (fmt != FramebufferTextureFormat::DEPTH24STENCIL8 && fmt != FramebufferTextureFormat::DEPTH_COMPONENT32F)
            {
                allBufs.push_back(GL_COLOR_ATTACHMENT0 + colorIndex);
                ++colorIndex;
            }
        }
        glDrawBuffers(static_cast<GLsizei>(allBufs.size()), allBufs.data());

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
        if (m_StagingTexture)
        {
            glDeleteTextures(1, &m_StagingTexture);
            m_StagingTexture = 0;
            m_StagingWidth = 0;
            m_StagingHeight = 0;
        }
    }

    void SSSRenderPass::EnsureStagingTexture(u32 width, u32 height)
    {
        if (m_StagingTexture && m_StagingWidth == width && m_StagingHeight == height)
        {
            return;
        }

        if (m_StagingTexture)
        {
            glDeleteTextures(1, &m_StagingTexture);
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &m_StagingTexture);
        glTextureStorage2D(m_StagingTexture, 1, GL_RGBA16F,
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glTextureParameteri(m_StagingTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_StagingTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_StagingTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_StagingTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        m_StagingWidth = width;
        m_StagingHeight = height;
    }
} // namespace OloEngine
