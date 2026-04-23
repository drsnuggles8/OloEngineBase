#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"

#include <glad/gl.h>

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
            ResetCommandBucket();
            return;
        }

        // Early out if no decal commands were submitted this frame
        if (m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        const bool useOIT = m_OITEnabled && m_OITBuffer && m_OITBuffer->GetFramebuffer() && m_OITShader;

        if (useOIT)
        {
            // Weighted-blended OIT forward-decal path. Decal draws accumulate
            // into the shared OITBuffer (RGBA16F accum + RG16F revealage) with
            // per-attachment blend funcs; `OITResolveRenderPass` composites
            // the result over the scene FB. Scene depth is still sampled from
            // the scene framebuffer so decal-world-position reconstruction
            // matches opaque geometry.
            Ref<Framebuffer> oitFB = m_OITBuffer->GetFramebuffer();

            m_OITBuffer->ClearForFrame();
            oitFB->Bind();

            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(GL_LEQUAL);
            RenderCommand::SetDepthMask(false);

            RenderCommand::SetBlendStateForAttachment(0, true);
            RenderCommand::SetBlendStateForAttachment(1, true);
            RenderCommand::SetBlendFuncForAttachment(0, GL_ONE, GL_ONE);
            RenderCommand::SetBlendFuncForAttachment(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);

            CommandDispatch::SetDecalOITShaderOverride(m_OITShader->GetRendererID());

            // Bind scene depth (for decal projection) — the OIT variant needs
            // the same `u_SceneDepth` at TEX_POSTPROCESS_DEPTH that the
            // forward variant uses.
            u32 const depthTextureID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTextureID);

            m_CommandBucket.SortCommands();
            auto& rendererAPI = RenderCommand::GetRendererAPI();
            m_CommandBucket.Execute(rendererAPI);

            CommandDispatch::SetDecalOITShaderOverride(0);

            if (m_AccumMarker)
                m_AccumMarker();

            RenderCommand::SetBlendStateForAttachment(0, false);
            RenderCommand::SetBlendStateForAttachment(1, false);
            RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            RenderCommand::SetBlendState(false);

            RenderCommand::SetDepthMask(true);
            RenderCommand::SetDepthFunc(GL_LESS);
            RenderCommand::BackCull();
            CommandDispatch::InvalidateRenderStateCache();

            oitFB->Unbind();

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

    void DecalRenderPass::ExecuteOnGBuffer(Ref<Framebuffer> writeTargetFB,
                                           Ref<Framebuffer> depthSamplingFB)
    {
        OLO_PROFILE_FUNCTION();

        if (!writeTargetFB || m_CommandBucket.GetCommandCount() == 0)
            return;
        if (!depthSamplingFB)
            depthSamplingFB = writeTargetFB;

        const u32 gbufferID = writeTargetFB->GetRendererID();
        writeTargetFB->Bind();

        // Bind the depth attachment of the *depth-sampling* framebuffer
        // (resolved single-sample in MSAA mode) at TEX_POSTPROCESS_DEPTH so
        // the decal fragment shader can reconstruct world positions via
        // plain sampler2D regardless of the write target's sample count.
        // Safe to sample the currently-bound depth since decal render state
        // disables depth writes.
        const u32 depthTextureID = depthSamplingFB->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTextureID);

        m_CommandBucket.SortCommands();

        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // Manual per-packet dispatch — each DrawDecalCommand::mode selects a
        // different drawbuffer + colorMask configuration so the decal only
        // writes into the intended G-Buffer channels.
        const GLenum drawAlbedoOnly[4] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_NONE };
        const GLenum drawNormalOnly[4] = { GL_NONE, GL_COLOR_ATTACHMENT1, GL_NONE, GL_NONE };
        const GLenum drawAlbedoAndNormal[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_NONE, GL_NONE };
        const GLenum drawEmissiveOnly[4] = { GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2, GL_NONE };
        const GLenum fullDrawBufs[4] = {
            GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3
        };

        u8 currentMode = 0xFF; // force first-time configure

        for (const auto* packet : m_CommandBucket.GetPackets())
        {
            if (!packet)
                continue;

            u8 packetMode = 0;
            if (packet->GetCommandType() == CommandType::DrawDecal)
            {
                const auto* decalCmd = packet->GetCommandData<DrawDecalCommand>();
                packetMode = decalCmd ? decalCmd->mode : 0;
            }

            if (packetMode != currentMode)
            {
                // Emissive mode additively accumulates into RT2.rgb so
                // overlapping emissive decals sum their contributions; all
                // other modes overwrite (the previous value is preserved for
                // channels outside the colour mask).
                const bool wantAdditive = (packetMode == 3);
                glBlendFunci(2, GL_ONE, GL_ONE);
                if (wantAdditive)
                    glEnablei(GL_BLEND, 2);
                else
                    glDisablei(GL_BLEND, 2);

                switch (packetMode)
                {
                    case 1: // Normal — RT1 only, xy writable, zw preserved
                        glNamedFramebufferDrawBuffers(gbufferID, 4, drawNormalOnly);
                        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(1, GL_TRUE, GL_TRUE, GL_FALSE, GL_FALSE);
                        glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(3, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        break;
                    case 2: // RMA — RT0.a + RT1.zw writable
                        glNamedFramebufferDrawBuffers(gbufferID, 4, drawAlbedoAndNormal);
                        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, GL_TRUE, GL_TRUE);
                        glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(3, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        break;
                    case 3: // Emissive — RT2.rgb writable, RT2.a (unlit flag) preserved
                        glNamedFramebufferDrawBuffers(gbufferID, 4, drawEmissiveOnly);
                        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
                        glColorMaski(3, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        break;
                    default: // Albedo — RT0.rgb writable, RT0.a preserved
                        glNamedFramebufferDrawBuffers(gbufferID, 4, drawAlbedoOnly);
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(2, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(3, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        break;
                }
                currentMode = packetMode;
            }

            packet->Execute(rendererAPI);
        }

        RenderCommand::SetDepthMask(true);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::BackCull();

        // Restore full colour masks + draw buffers for subsequent passes.
        for (GLuint rt = 0; rt < 4; ++rt)
            glColorMaski(rt, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glNamedFramebufferDrawBuffers(gbufferID, 4, fullDrawBufs);

        // Restore RT2 blend state — emissive additive blending leaks into
        // the next pass otherwise (observed as SSAO / GTAO darkening the
        // emissive channel during composite).
        glDisablei(GL_BLEND, 2);

        writeTargetFB->Unbind();

        // Bucket is drained here — the regular graph-scheduled Execute()
        // will observe an empty bucket and no-op this frame.
        ResetCommandBucket();
    }
} // namespace OloEngine
