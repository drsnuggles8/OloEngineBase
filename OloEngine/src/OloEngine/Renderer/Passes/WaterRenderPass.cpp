#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    WaterRenderPass::WaterRenderPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("WaterRenderPass");
        OLO_CORE_INFO("Creating WaterRenderPass.");
    }

    WaterRenderPass::~WaterRenderPass()
    {
        if (m_RefractionTextureID != 0)
        {
            glDeleteTextures(1, &m_RefractionTextureID);
            m_RefractionTextureID = 0;
        }
    }

    void WaterRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // No own framebuffer — this pass renders into the ScenePass target
    }

    void WaterRenderPass::EnsureRefractionTexture(u32 width, u32 height)
    {
        if (m_RefractionTextureID != 0 && m_RefractionWidth == width && m_RefractionHeight == height)
        {
            return;
        }

        if (m_RefractionTextureID != 0)
        {
            glDeleteTextures(1, &m_RefractionTextureID);
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &m_RefractionTextureID);
        glTextureStorage2D(m_RefractionTextureID, 1, GL_RGBA16F,
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        m_RefractionWidth = width;
        m_RefractionHeight = height;
    }

    void WaterRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_SceneFramebuffer)
        {
            ResetCommandBucket();
            return;
        }

        // Early out if no water commands were submitted this frame
        if (m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        u32 const fbWidth = m_SceneFramebuffer->GetSpecification().Width;
        u32 const fbHeight = m_SceneFramebuffer->GetSpecification().Height;

        // Guard against zero-sized framebuffers (minimized window, etc.)
        if (fbWidth == 0 || fbHeight == 0)
        {
            if (m_RefractionTextureID != 0)
            {
                glDeleteTextures(1, &m_RefractionTextureID);
                m_RefractionTextureID = 0;
                m_RefractionWidth = 0;
                m_RefractionHeight = 0;
            }
            ResetCommandBucket();
            return;
        }

        // Copy scene color for refraction (before water renders over it)
        u32 const sceneColorID = m_SceneFramebuffer->GetColorAttachmentRendererID(0);
        EnsureRefractionTexture(fbWidth, fbHeight);
        glCopyImageSubData(
            sceneColorID, GL_TEXTURE_2D, 0, 0, 0, 0,
            m_RefractionTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
            static_cast<GLsizei>(fbWidth), static_cast<GLsizei>(fbHeight), 1);

        m_SceneFramebuffer->Bind();

        // Bind scene depth for depth softening and shoreline foam
        u32 const depthTextureID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, depthTextureID);

        // Bind refraction color copy
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, m_RefractionTextureID);

        // Bind scene view-space normals for SSR ray marching
        u32 const normalsTextureID = m_SceneFramebuffer->GetColorAttachmentRendererID(2);
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, normalsTextureID);

        // Sort and dispatch water commands through the command bucket
        m_CommandBucket.SortCommands();

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        m_CommandBucket.Execute(rendererAPI);

        // Restore render state after water (water uses blending + depth write off)
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::BackCull();
        CommandDispatch::InvalidateRenderStateCache();

        m_SceneFramebuffer->Unbind();

        // Reset bucket for next frame
        ResetCommandBucket();
    }

    Ref<Framebuffer> WaterRenderPass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        // Return the ScenePass framebuffer since that's where we render
        return m_SceneFramebuffer;
    }

    void WaterRenderPass::SetSceneFramebuffer(const Ref<Framebuffer>& fb)
    {
        OLO_PROFILE_FUNCTION();
        m_SceneFramebuffer = fb;
    }

    void WaterRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer — dimensions tracked for consistency
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void WaterRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void WaterRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer to reset
    }
} // namespace OloEngine
