#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ParticleRenderPass.h"
#include "OloEngine/Particle/ParticleBatchRenderer.h"
#include "OloEngine/Renderer/Renderer.h"

#include <glad/gl.h>

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
        // No own framebuffer — this pass renders into the ScenePass target (classic)
        // or into the OITBuffer (WB-OIT path).
    }

    void ParticleRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_RenderCallback || !m_SceneFramebuffer)
        {
            return;
        }

        const bool useOIT = m_OITEnabled && m_OITBuffer && m_OITBuffer->GetFramebuffer();

        if (useOIT)
        {
            // Weighted-blended OIT path. Transparent particles
            // accumulate into OITBuffer and OITResolveRenderPass composites
            // the result over the scene FB.
            Ref<Framebuffer> oitFB = m_OITBuffer->GetFramebuffer();

            // Fresh per-frame accumulation state.
            m_OITBuffer->ClearForFrame();

            oitFB->Bind();

            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(GL_LEQUAL);
            RenderCommand::SetDepthMask(false);

            // Per-attachment blend state: both enabled, different factors.
            RenderCommand::SetBlendStateForAttachment(0, true);                           // accum
            RenderCommand::SetBlendStateForAttachment(1, true);                           // revealage
            RenderCommand::SetBlendFuncForAttachment(0, GL_ONE, GL_ONE);                  // additive
            RenderCommand::SetBlendFuncForAttachment(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // multiplicative

            ParticleBatchRenderer::SetOITMode(true);

            m_RenderCallback();

            ParticleBatchRenderer::SetOITMode(false);

            // Signal OITResolvePass that it has fresh accumulated content.
            if (m_AccumMarker)
                m_AccumMarker();

            // Restore global blend state so subsequent passes don't inherit
            // the per-attachment WB-OIT factors.
            RenderCommand::SetBlendStateForAttachment(0, false);
            RenderCommand::SetBlendStateForAttachment(1, false);
            RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            RenderCommand::SetBlendState(false);

            RenderCommand::SetDepthFunc(GL_LESS);
            RenderCommand::SetDepthMask(true);

            oitFB->Unbind();
        }
        else
        {
            // Classic alpha-blended path.
            m_SceneFramebuffer->Bind();

            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(GL_LEQUAL);
            RenderCommand::SetDepthMask(false);

            // Enable blending only on draw buffer 0 (color).
            // Draw buffer 1 is RED_INTEGER (entity ID); Draw buffer 2 is view-space normals.
            RenderCommand::SetBlendStateForAttachment(0, true);
            RenderCommand::SetBlendStateForAttachment(1, false);
            RenderCommand::SetBlendStateForAttachment(2, false);
            RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            m_RenderCallback();

            RenderCommand::SetDepthFunc(GL_LESS);
            RenderCommand::SetDepthMask(true);
            RenderCommand::SetBlendStateForAttachment(0, false);
            RenderCommand::SetBlendState(false);

            m_SceneFramebuffer->Unbind();
        }

        m_RenderCallback = nullptr;
    }

    Ref<Framebuffer> ParticleRenderPass::GetTarget() const
    {
        // Always report the scene FB as our target: OIT composite lands
        // back into the scene FB via OITResolvePass.
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
