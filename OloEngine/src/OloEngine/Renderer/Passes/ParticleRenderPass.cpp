#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGCommandContext.h"
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
        // No own framebuffer — this pass renders into the ScenePass target
        // (classic) or into the graph-owned OIT framebuffer (WB-OIT path).

        // Resource-aware RDG: in the OIT path we write the accumulation and
        // revealage buffers; in the classic path we write directly into SceneColor.
        // Declaring all three lets the hazard validator derive the RAW ordering
        // edge to OITResolveRenderPass (which reads OITAccum/OITRevealage).
        // Phase F slice 32 — also declare SceneColor read so the WaterPass →
        // ParticlePass RAW ordering edge is derived (WaterPass writes SceneColor,
        // this pass reads-then-writes it).
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::OITAccum, ResourceHandle::Kind::Texture2D);
        DeclareWrite(ResourceNames::OITRevealage, ResourceHandle::Kind::Texture2D);
        DeclareWrite(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
    }

    void ParticleRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void ParticleRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 36 — self-resolving SceneColor: look up directly
        // from the render graph blackboard so no per-frame side-channel
        // setter call is needed from EndScene().
        if (const auto* board = context.GetBlackboard())
        {
            if (board->SceneColor.IsValid())
            {
                if (auto resolvedSceneFB = context.ResolveFramebuffer(board->SceneColor))
                    m_SceneFramebuffer = resolvedSceneFB;
            }
        }

        if (!m_RenderCallback || !m_SceneFramebuffer)
        {
            return;
        }

        Ref<Framebuffer> oitFramebuffer;
        if (m_OITEnabled)
        {
            if (const auto* board = context.GetBlackboard(); board && board->OITAccum.IsValid())
                oitFramebuffer = context.ResolveFramebuffer(board->OITAccum);
        }

        const bool useOIT = m_OITEnabled && oitFramebuffer;

        if (useOIT)
        {
            // Weighted-blended OIT path. Transparent particles
            // accumulate into the graph-owned OIT target and OITResolveRenderPass composites
            // the result over the scene FB.
            Ref<Framebuffer> oitFB = oitFramebuffer;

            oitFB->Bind();

            context.SetDepthTest(true);
            RenderCommand::SetDepthFunc(GL_LEQUAL);
            context.SetDepthMask(false);

            // Per-attachment blend state: both enabled, different factors.
            RenderCommand::SetBlendStateForAttachment(0, true);                           // accum
            RenderCommand::SetBlendStateForAttachment(1, true);                           // revealage
            RenderCommand::SetBlendFuncForAttachment(0, GL_ONE, GL_ONE);                  // additive
            RenderCommand::SetBlendFuncForAttachment(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // multiplicative

            ParticleBatchRenderer::SetOITMode(true);

            m_RenderCallback();

            ParticleBatchRenderer::SetOITMode(false);

            // Restore global blend state so subsequent passes don't inherit
            // the per-attachment WB-OIT factors.
            RenderCommand::SetBlendStateForAttachment(0, false);
            RenderCommand::SetBlendStateForAttachment(1, false);
            RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            context.SetBlendState(false);

            RenderCommand::SetDepthFunc(GL_LESS);
            context.SetDepthMask(true);

            oitFB->Unbind();
        }
        else
        {
            // Classic alpha-blended path.
            m_SceneFramebuffer->Bind();

            context.SetDepthTest(true);
            RenderCommand::SetDepthFunc(GL_LEQUAL);
            context.SetDepthMask(false);

            // Enable blending only on draw buffer 0 (color).
            // Draw buffer 1 is RED_INTEGER (entity ID); Draw buffer 2 is view-space normals.
            RenderCommand::SetBlendStateForAttachment(0, true);
            RenderCommand::SetBlendStateForAttachment(1, false);
            RenderCommand::SetBlendStateForAttachment(2, false);
            RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            m_RenderCallback();

            RenderCommand::SetDepthFunc(GL_LESS);
            context.SetDepthMask(true);
            RenderCommand::SetBlendStateForAttachment(0, false);
            context.SetBlendState(false);

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
