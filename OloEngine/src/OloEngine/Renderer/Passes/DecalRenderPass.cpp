#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"

#include <array>

namespace OloEngine
{
    DecalRenderPass::DecalRenderPass()
    {
        SetName("DecalRenderPass");
        OLO_CORE_INFO("Creating DecalRenderPass.");
    }

    void DecalRenderPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedOITFramebuffer = {};
        m_SelectedSceneDepthTexture = {};

        if (m_CommandBucket.GetCommandCount() == 0)
            return;

        const bool hasProjectionDepth = board.Scene.SceneDepth.IsValid();
        const bool writesOIT = m_OITEnabled && (board.OIT.OITAccum.IsValid() || board.OIT.OITRevealage.IsValid());
        const bool writesSceneColor = !m_OITEnabled && board.Scene.SceneColor.IsValid();

        if (hasProjectionDepth && (writesOIT || writesSceneColor))
        {
            m_SelectedSceneDepthTexture = board.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }

        if (m_OITEnabled)
        {
            if (board.Scene.SceneColor.IsValid())
                SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
            if (board.OIT.OITBuffer.IsValid())
                m_SelectedOITFramebuffer = board.OIT.OITBuffer;
            if (board.OIT.OITDepthAttachment.IsValid())
            {
                [[maybe_unused]] const auto oitDepthRead = builder.Read(board.OIT.OITDepthAttachment, RGReadUsage::RenderTargetRead);
            }
            // Inter-pass RMW into the OIT accum/revealage targets cleared
            // by OITPreparePass. The prior version's RenderTargetRead is the
            // input side; WriteNewVersion renames the output, so the
            // validator sees Read("OITAccum") → Write("OITAccum@DecalPass")
            // with no same-pass feedback loop and downstream readers pick up
            // the new version via the resource-name version map.
            constexpr std::string_view decalOITVersionTag = "DecalPass";
            if (board.OIT.OITAccum.IsValid())
            {
                [[maybe_unused]] const auto oitAccumRead = builder.Read(board.OIT.OITAccum, RGReadUsage::RenderTargetRead);
                [[maybe_unused]] const auto oitAccumNew =
                    builder.WriteNewVersion(board.OIT.OITAccum, RGWriteUsage::RenderTarget, decalOITVersionTag);
            }
            if (board.OIT.OITRevealage.IsValid())
            {
                [[maybe_unused]] const auto oitRevealageRead = builder.Read(board.OIT.OITRevealage, RGReadUsage::RenderTargetRead);
                [[maybe_unused]] const auto oitRevealageNew =
                    builder.WriteNewVersion(board.OIT.OITRevealage, RGWriteUsage::RenderTarget, decalOITVersionTag);
            }

            // Pin OIT writer-chain ordering against OITPreparePass's clear.
            // Without this edge the Decal RMW vs OITPrepare Clear WAW chain is
            // registration-order-sensitive. ParticleRenderPass mirrors this
            // declaration; the contributor-to-contributor edge from Particle to
            // Decal is now derived by Particle's Setup via
            // builder.DependsOnPreviousWriter("OITAccum").
            if (board.OIT.OITAccum.IsValid() || board.OIT.OITRevealage.IsValid())
                builder.DependsOnPass("OITPreparePass");
        }
        else if (board.Scene.SceneColor.IsValid())
        {
            // Inter-pass RMW: bind the prior SceneColor version as the
            // render target (so Execute resolves the same physical FB via
            // GetPrimaryInputFramebufferHandle) and advertise a new version
            // as this pass's logical output. `WriteNewVersion` republishes
            // the base attachment views as versioned siblings; see
            // ForwardOverlayRenderPass for the rationale.
            SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
            [[maybe_unused]] const auto sceneColorRead = builder.Read(board.Scene.SceneColor, RGReadUsage::RenderTargetRead);
            constexpr std::string_view decalSceneColorVersionTag = "DecalPass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(board.Scene.SceneColor, RGWriteUsage::RenderTarget, decalSceneColorVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }
        else
        {
            // No additional handling required.
        }
    }

    void DecalRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Per-pass command capture (issue #463): register this pass and snapshot its
        // submission-order bucket BEFORE any early-return below, so the decal pass
        // always appears in the frame breakdown's per-pass list. In the deferred
        // path the opaque decals were already drained into the G-Buffer by the
        // separate DeferredOpaqueDecalPass node (which resets the bucket when no
        // transparent decals remain), so this capture sees only the still-queued
        // (transparent) decals — captured under this pass's single name.
        auto& captureManager = FrameCaptureManager::GetInstance();
        const bool capturing = captureManager.IsCapturing();
        if (capturing)
        {
            captureManager.BeginPass(GetName());
            captureManager.OnPreSort(m_CommandBucket);
        }

        // Resolve the setup-selected scene framebuffer instead of replaying
        // a blackboard lookup ladder at execute time.
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_SceneFramebuffer = resolvedSceneFB;
        }

        // Phase F slice 36 / Phase H follow-up — self-resolving SceneDepth
        // (for decal projection). No raw framebuffer fallback; if the
        // blackboard is absent the depth slot is left unbound (acceptable for
        // headless / unit-test contexts where no geometry is dispatched).
        RHI::ResourceHandle depthTextureID{};
        if (m_SelectedSceneDepthTexture.IsValid())
            depthTextureID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);

        // Detector-only guard: captures GL state at entry and on destruction
        // diffs against exit state, logging any field this pass failed to
        // restore. The explicit restore calls further down still perform the
        // actual restoration (the current GLStateGuard only detects leaks,
        // it does not roll back).
        GLStateGuard guard("DecalRenderPass");

        // Helper: decide whether a packet should be drained by *this*
        // (the graph-scheduled) Execute. In the Deferred path the opaque
        // decals were already written into the G-Buffer by
        // `ExecuteOnGBuffer`, so here we only want the `transparent == 1`
        // packets that need to composite over the already-lit scene colour.
        // In Forward / Forward+, every packet is owned by this pass.
        const bool opaqueAlreadyDrained = m_OpaqueDecalsDrained;
        m_OpaqueDecalsDrained = false; // one-shot
        auto shouldDrawHere = [opaqueAlreadyDrained](const CommandPacket* p) -> bool
        {
            if (!p)
                return false;
            if (!opaqueAlreadyDrained)
                return true;
            if (p->GetCommandType() != CommandType::DrawDecal)
                return true; // defensive — pass-through unknown commands
            const auto* dc = p->GetCommandData<DrawDecalCommand>();
            return dc && dc->transparent != 0;
        };

        if (!m_SceneFramebuffer)
        {
            ResetCommandBucket();
            return;
        }

        // Early out if no decal commands were submitted this frame, or if
        // every queued packet was already drained by ExecuteOnGBuffer (pure
        // opaque deferred scene with no transparent overlays).
        if (m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        bool hasAnyToDraw = false;
        for (const auto* packet : m_CommandBucket.GetPackets())
        {
            if (shouldDrawHere(packet))
            {
                hasAnyToDraw = true;
                break;
            }
        }
        if (!hasAnyToDraw)
        {
            ResetCommandBucket();
            return;
        }

        Ref<Framebuffer> oitFramebuffer;
        if (m_OITEnabled && m_SelectedOITFramebuffer.IsValid())
            oitFramebuffer = context.ResolveFramebuffer(m_SelectedOITFramebuffer);

        if (const bool useOIT = m_OITEnabled && oitFramebuffer && m_OITShader; useOIT)
        {
            // Weighted-blended OIT forward-decal path. Decal draws accumulate
            // into the shared graph-owned OIT framebuffer (RGBA16F accum + RG16F revealage) with
            // per-attachment blend funcs; `OITResolveRenderPass` composites
            // the result over the scene FB. Scene depth is still sampled from
            // the scene framebuffer so decal-world-position reconstruction
            // matches opaque geometry.
            Ref<Framebuffer> oitFB = oitFramebuffer;
            oitFB->Bind();

            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::LessOrEqual);
            RenderCommand::SetDepthMask(false);

            RenderCommand::SetBlendStateForAttachment(0, true);
            RenderCommand::SetBlendStateForAttachment(1, true);
            RenderCommand::SetBlendFuncForAttachment(0, RHI::BlendFactor::One, RHI::BlendFactor::One);
            RenderCommand::SetBlendFuncForAttachment(1, RHI::BlendFactor::Zero, RHI::BlendFactor::OneMinusSrcColor);

            // Install Decal_OIT program override directly on each queued
            // DrawDecalCommand packet. Keeping the override on the command
            // (instead of a global on CommandDispatch) preserves the
            // stateless, replay-safe contract of the bucket.
            const RHI::ResourceHandle decalOITProgram = m_OITShader->GetRHIHandle();
            for (CommandPacket* packet : m_CommandBucket.GetPackets())
            {
                if (!packet || packet->GetCommandType() != CommandType::DrawDecal)
                    continue;
                if (auto* cmd = packet->GetCommandData<DrawDecalCommand>())
                    cmd->oitProgramOverride = decalOITProgram;
            }

            // Bind scene depth (for decal projection) — the OIT variant needs
            // the same `u_SceneDepth` at TEX_POSTPROCESS_DEPTH that the
            // forward variant uses.
            // PUBLISH, not bind: this is pass-level state for whatever the
            // command bucket dispatches below, and each of those draws binds its
            // own program. BindTextureOrHeapOffset would fork on whatever program
            // happens to be in flight here — never the decal shaders — so the seam
            // must stage the offset AND issue the bind (issue #691 Phase 3).
            HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTextureID,
                                                     RHI::HeapSlotLifetime::FrameTransient);
            HeapBinding::FlushOffsets();

            m_CommandBucket.SortCommands();
            if (capturing)
                captureManager.OnPostSort(m_CommandBucket);
            auto& rendererAPI = RenderCommand::GetRendererAPI();
            for (const auto* packet : m_CommandBucket.GetPackets())
            {
                if (shouldDrawHere(packet))
                    packet->Execute(rendererAPI);
            }

            RenderCommand::SetBlendStateForAttachment(0, false);
            RenderCommand::SetBlendStateForAttachment(1, false);
            RenderCommand::SetBlendFunc(RHI::BlendFactor::SrcAlpha, RHI::BlendFactor::OneMinusSrcAlpha);
            context.SetBlendState(false);

            context.SetDepthMask(true);
            RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
            RenderCommand::BackCull();
            CommandDispatch::InvalidateRenderStateCache();

            oitFB->Unbind();

            ResetCommandBucket();
            return;
        }

        m_SceneFramebuffer->Bind();

        // Bind scene depth texture for decal projection (before dispatching commands).
        // Published, not bound — see the OIT variant above.
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTextureID,
                                                 RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::FlushOffsets();

        // Sort and dispatch decal commands through the command bucket
        m_CommandBucket.SortCommands();

        if (capturing)
            captureManager.OnPostSort(m_CommandBucket);

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        for (const auto* packet : m_CommandBucket.GetPackets())
        {
            if (shouldDrawHere(packet))
                packet->Execute(rendererAPI);
        }

        // Restore render state after decals
        context.SetDepthMask(true);
        context.SetBlendState(false);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
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
        // Clear the opaque-drain guard so a graph reset (resize, path switch,
        // hot-reload) doesn't leave it latched to "true" from the previous
        // frame, which would cause ExecuteOnGBuffer to skip all opaque decals.
        m_OpaqueDecalsDrained = false;
        m_SelectedOITFramebuffer = {};
        m_SelectedSceneDepthTexture = {};
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

        const RHI::ResourceHandle gbufferID = writeTargetFB->GetRHIHandle();
        writeTargetFB->Bind();

        // Bind the depth attachment of the *depth-sampling* framebuffer
        // (resolved single-sample in MSAA mode) at TEX_POSTPROCESS_DEPTH so
        // the decal fragment shader can reconstruct world positions via
        // plain sampler2D regardless of the write target's sample count.
        // Safe to sample the currently-bound depth since decal render state
        // disables depth writes.
        const RHI::ResourceHandle depthTexture = depthSamplingFB->GetDepthAttachmentHandle();
        // Published, not bound — see the OIT variant above.
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTexture,
                                                 RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::FlushOffsets();

        m_CommandBucket.SortCommands();

        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // Manual per-packet dispatch — each DrawDecalCommand::mode selects a
        // different drawbuffer + colorMask configuration so the decal only
        // writes into the intended G-Buffer channels. Arrays are sized to
        // `GBuffer::Count` so RT4 (entity ID) stays unwritten during decal
        // rendering — decals must not stamp their own pickability over the
        // underlying mesh's entity ID.
        // RHI::NoAttachment is the neutral spelling of "this draw slot writes
        // nowhere". It exists precisely for these lists: it is not an attachment
        // index, and both backends need it (GL_NONE / VK_ATTACHMENT_UNUSED).
        constexpr sizet kGBufferCount = static_cast<sizet>(std::to_underlying(GBuffer::Count));
        constexpr u32 kNone = RHI::NoAttachment;
        const std::array<u32, kGBufferCount> drawAlbedoOnly = { 0, kNone, kNone, kNone, kNone };
        const std::array<u32, kGBufferCount> drawNormalOnly = { kNone, 1, kNone, kNone, kNone };
        const std::array<u32, kGBufferCount> drawAlbedoAndNormal = { 0, 1, kNone, kNone, kNone };
        const std::array<u32, kGBufferCount> drawEmissiveOnly = { kNone, kNone, 2, kNone, kNone };
        const std::array<u32, kGBufferCount> fullDrawBufs = { 0, 1, 2, 3, 4 };

        using DecalMode = DrawDecalCommand::DecalMode;
        // Sentinel outside the valid enumerator range — forces the first
        // packet to reconfigure the draw buffers + masks.
        auto currentMode = static_cast<DecalMode>(0xFF);
        bool anyTransparentQueued = false;

        for (const auto* packet : m_CommandBucket.GetPackets())
        {
            if (!packet)
                continue;

            DecalMode packetMode = DecalMode::Albedo;
            bool packetTransparent = false;
            if (packet->GetCommandType() == CommandType::DrawDecal)
            {
                const auto* decalCmd = packet->GetCommandData<DrawDecalCommand>();
                packetMode = decalCmd ? decalCmd->mode : DecalMode::Albedo;
                packetTransparent = decalCmd && decalCmd->transparent != 0;
            }

            // Transparent decals don't belong in the G-Buffer overlay drain;
            // leave them for the graph-scheduled Execute() to composite over
            // the lit scene colour after DeferredLightingPass.
            if (packetTransparent)
            {
                anyTransparentQueued = true;
                continue;
            }

            if (packetMode != currentMode)
            {
                // Emissive mode additively accumulates into RT2.rgb so
                // overlapping emissive decals sum their contributions; all
                // other modes overwrite (the previous value is preserved for
                // channels outside the colour mask).
                const bool wantAdditive = (packetMode == DecalMode::Emissive);
                RenderCommand::SetBlendFuncForAttachment(2, RHI::BlendFactor::One, RHI::BlendFactor::One);
                RenderCommand::SetBlendStateForAttachment(2, wantAdditive);

                switch (packetMode)
                {
                    case DecalMode::Normal: // RT1 only, xy writable, zw preserved
                        RenderCommand::SetFramebufferDrawAttachments(gbufferID, drawNormalOnly);
                        RenderCommand::SetColorMaskForAttachment(0, false, false, false, false);
                        RenderCommand::SetColorMaskForAttachment(1, true, true, false, false);
                        RenderCommand::SetColorMaskForAttachment(2, false, false, false, false);
                        RenderCommand::SetColorMaskForAttachment(3, false, false, false, false);
                        break;
                    case DecalMode::RMA: // RT0.a + RT1.zw writable
                        RenderCommand::SetFramebufferDrawAttachments(gbufferID, drawAlbedoAndNormal);
                        RenderCommand::SetColorMaskForAttachment(0, false, false, false, true);
                        RenderCommand::SetColorMaskForAttachment(1, false, false, true, true);
                        RenderCommand::SetColorMaskForAttachment(2, false, false, false, false);
                        RenderCommand::SetColorMaskForAttachment(3, false, false, false, false);
                        break;
                    case DecalMode::Emissive: // RT2.rgb writable, RT2.a (unlit flag) preserved
                        RenderCommand::SetFramebufferDrawAttachments(gbufferID, drawEmissiveOnly);
                        RenderCommand::SetColorMaskForAttachment(0, false, false, false, false);
                        RenderCommand::SetColorMaskForAttachment(1, false, false, false, false);
                        RenderCommand::SetColorMaskForAttachment(2, true, true, true, false);
                        RenderCommand::SetColorMaskForAttachment(3, false, false, false, false);
                        break;
                    case DecalMode::Albedo:
                    default: // RT0.rgb writable, RT0.a preserved
                        RenderCommand::SetFramebufferDrawAttachments(gbufferID, drawAlbedoOnly);
                        RenderCommand::SetColorMaskForAttachment(0, true, true, true, false);
                        RenderCommand::SetColorMaskForAttachment(1, false, false, false, false);
                        RenderCommand::SetColorMaskForAttachment(2, false, false, false, false);
                        RenderCommand::SetColorMaskForAttachment(3, false, false, false, false);
                        break;
                }
                currentMode = packetMode;

                // The per-attachment state above bypasses our cached render-state
                // tracking; invalidate so the next dispatched packet
                // re-applies its POD state instead of skipping as a no-op.
                CommandDispatch::InvalidateRenderStateCache();
            }

            packet->Execute(rendererAPI);
        }

        RenderCommand::SetDepthMask(true);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        RenderCommand::BackCull();

        // Restore full colour masks + draw buffers for subsequent passes.
        // Only the RGBA-colour attachments (RT0-RT3) need a colour-mask
        // restore — RT4 is integer (R32I, entity ID) and a per-attachment
        // colour mask is a no-op there (the mask only applies to
        // floating-point / normalised outputs).
        for (u32 rt = 0; rt < 4; ++rt)
            RenderCommand::SetColorMaskForAttachment(rt, true, true, true, true);
        RenderCommand::SetFramebufferDrawAttachments(gbufferID, fullDrawBufs);

        // Restore RT2 blend state — emissive additive blending leaks into
        // the next pass otherwise (observed as SSAO / GTAO darkening the
        // emissive channel during composite).
        RenderCommand::SetBlendStateForAttachment(2, false);

        // The per-attachment mask / blend / draw-buffer calls above bypass the
        // cached render-state tracking; invalidate so the
        // next pass's first packet reapplies its POD state instead of being
        // elided as a no-op against the now-stale cache snapshot.
        CommandDispatch::InvalidateRenderStateCache();

        writeTargetFB->Unbind();

        // If any transparent decals are still queued, preserve the bucket so
        // the graph-scheduled Execute() (running after DeferredLightingPass)
        // can composite them over the lit scene colour. Mark the opaque
        // drain so Execute knows to skip already-rendered opaque packets.
        // Otherwise drain the bucket here — Execute will early-out on empty.
        if (anyTransparentQueued)
        {
            m_OpaqueDecalsDrained = true;
        }
        else
        {
            // Bucket is drained here — the regular graph-scheduled Execute()
            // will observe an empty bucket and no-op this frame.
            ResetCommandBucket();
        }
    }
} // namespace OloEngine
