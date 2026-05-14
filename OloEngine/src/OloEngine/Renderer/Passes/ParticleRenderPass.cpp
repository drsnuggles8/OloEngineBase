#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
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

    void ParticleRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedOITFramebuffer = {};

        if (!m_RenderCallback)
            return;

        if (m_OITEnabled)
        {
            if (blackboard.Scene.SceneColor.IsValid())
                SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
            if (blackboard.OIT.OITBuffer.IsValid())
                m_SelectedOITFramebuffer = blackboard.OIT.OITBuffer;
            if (blackboard.OIT.OITDepthAttachment.IsValid())
            {
                [[maybe_unused]] const auto oitDepthRead = builder.Read(blackboard.OIT.OITDepthAttachment, RGReadUsage::RenderTargetRead);
            }
            // Inter-pass RMW into the OIT accum/revealage targets (cleared
            // by OITPreparePass, possibly already written by Decal). Read the
            // prior version for blending, then advertise a renamed output via
            // WriteNewVersion so the validator does not see a same-pass loop.
            constexpr std::string_view particleOITVersionTag = "ParticlePass";
            if (blackboard.OIT.OITAccum.IsValid())
            {
                [[maybe_unused]] const auto oitAccumRead = builder.Read(blackboard.OIT.OITAccum, RGReadUsage::RenderTargetRead);
                [[maybe_unused]] const auto oitAccumNew =
                    builder.WriteNewVersion(blackboard.OIT.OITAccum, RGWriteUsage::RenderTarget, particleOITVersionTag);
            }
            if (blackboard.OIT.OITRevealage.IsValid())
            {
                [[maybe_unused]] const auto oitRevealageRead = builder.Read(blackboard.OIT.OITRevealage, RGReadUsage::RenderTargetRead);
                [[maybe_unused]] const auto oitRevealageNew =
                    builder.WriteNewVersion(blackboard.OIT.OITRevealage, RGWriteUsage::RenderTarget, particleOITVersionTag);
            }

            // Pin OIT writer-chain ordering: OITPreparePass clears the targets,
            // then contributors RMW into them in a chosen order. WB-OIT is
            // mathematically commutative across contributors, so the chain just
            // disambiguates the WAW order (which is registration-order-sensitive
            // without explicit edges). Particle depends on OITPrepare (declared
            // explicitly so the no-Decal case is still pinned) and on the
            // previous OITAccum writer (the earlier contributor, e.g. Decal in
            // OIT mode) discovered via the graph's last-writer tracker.
            if (blackboard.OIT.OITAccum.IsValid() || blackboard.OIT.OITRevealage.IsValid())
                builder.DependsOnPass("OITPreparePass");
            builder.DependsOnPreviousWriter(ResourceNames::OITAccum);
        }
        else if (blackboard.Scene.SceneColor.IsValid())
        {
            // Inter-pass RMW: read the prior SceneColor version, then
            // advertise a renamed output via WriteNewVersion so the
            // validator does not see a same-pass feedback loop.
            SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
            [[maybe_unused]] const auto sceneColorRead = builder.Read(blackboard.Scene.SceneColor, RGReadUsage::RenderTargetRead);
            constexpr std::string_view particleSceneColorVersionTag = "ParticlePass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(blackboard.Scene.SceneColor, RGWriteUsage::RenderTarget, particleSceneColorVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }
    }

    void ParticleRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // No own framebuffer — this pass renders into the ScenePass target
        // (classic) or into the graph-owned OIT framebuffer (WB-OIT path).
    }

    void ParticleRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Resolve the setup-selected scene framebuffer instead of replaying
        // a blackboard lookup ladder at execute time.
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_SceneFramebuffer = resolvedSceneFB;
        }

        if (!m_RenderCallback || !m_SceneFramebuffer)
        {
            return;
        }

        Ref<Framebuffer> oitFramebuffer;
        if (m_OITEnabled && m_SelectedOITFramebuffer.IsValid())
            oitFramebuffer = context.ResolveFramebuffer(m_SelectedOITFramebuffer);

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
        m_SelectedOITFramebuffer = {};
        // No own framebuffer to reset
    }
} // namespace OloEngine
