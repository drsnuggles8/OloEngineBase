#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    DecalRenderPass::DecalRenderPass()
    {
        SetName("DecalRenderPass");
        OLO_CORE_INFO("Creating DecalRenderPass.");
    }

    void DecalRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // No own framebuffer — this pass renders into the ScenePass target
    }

    void DecalRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_SceneFramebuffer)
        {
            return;
        }

        // Early out if no decal commands were submitted this frame
        if (m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        m_SceneFramebuffer->Bind();

        // Bind scene depth texture for decal projection (before dispatching commands)
        u32 depthTextureID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTextureID);

        // Sort and dispatch decal commands through the command bucket
        m_CommandBucket.SortCommands();

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        m_CommandBucket.Execute(rendererAPI);

        // Restore render state after decals
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::BackCull();

        m_SceneFramebuffer->Unbind();

        // Reset bucket for next frame
        ResetCommandBucket();
    }

    Ref<Framebuffer> DecalRenderPass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        // Return the ScenePass framebuffer since that's where we render
        return m_SceneFramebuffer;
    }

    void DecalRenderPass::SetSceneFramebuffer(const Ref<Framebuffer>& fb)
    {
        OLO_PROFILE_FUNCTION();
        m_SceneFramebuffer = fb;
    }

    void DecalRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer — dimensions tracked for consistency
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void DecalRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void DecalRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer to reset
    }
} // namespace OloEngine
