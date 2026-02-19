#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"

namespace OloEngine
{
    ParticleRenderPass::ParticleRenderPass()
    {
        SetName("ParticleRenderPass");
    }

    void ParticleRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // No own framebuffer — this pass renders into the ScenePass target
    }

    void ParticleRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_RenderCallback || !m_SceneFramebuffer)
        {
            return;
        }

        m_SceneFramebuffer->Bind();

        // Transparent particles: read depth buffer (no write), enable blending
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthFunc(GL_LEQUAL);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(true);
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        m_RenderCallback();

        // Restore defaults for subsequent passes
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetBlendState(false);

        // Clear callback to prevent stale captures across frames
        m_RenderCallback = nullptr;
    }

    Ref<Framebuffer> ParticleRenderPass::GetTarget() const
    {
        // Return the ScenePass framebuffer since that's where we render
        return m_SceneFramebuffer;
    }

    void ParticleRenderPass::SetSceneFramebuffer(const Ref<Framebuffer>& fb)
    {
        m_SceneFramebuffer = fb;
    }

    void ParticleRenderPass::SetRenderCallback(RenderCallback callback)
    {
        m_RenderCallback = std::move(callback);
    }

    void ParticleRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        // No own framebuffer — dimensions tracked for consistency
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ParticleRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ParticleRenderPass::OnReset()
    {
        // No own framebuffer to reset
    }
} // namespace OloEngine
