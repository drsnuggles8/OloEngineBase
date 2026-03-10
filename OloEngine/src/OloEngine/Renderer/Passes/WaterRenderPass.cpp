#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Renderer.h"

namespace OloEngine
{
    WaterRenderPass::WaterRenderPass()
    {
        SetName("WaterRenderPass");
        OLO_CORE_INFO("Creating WaterRenderPass.");
    }

    void WaterRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // No own framebuffer — this pass renders into the ScenePass target
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

        m_SceneFramebuffer->Bind();

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
