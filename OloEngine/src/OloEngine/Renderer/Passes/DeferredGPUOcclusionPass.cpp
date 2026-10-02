#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredGPUOcclusionPass.h"
#include "OloEngine/Renderer/Passes/GBufferWriteDeclarations.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"

#include <array>

namespace OloEngine
{
    DeferredGPUOcclusionPass::DeferredGPUOcclusionPass()
    {
        SetName("DeferredGPUOcclusionPass");
        OLO_CORE_INFO("Creating DeferredGPUOcclusionPass.");
    }

    void DeferredGPUOcclusionPass::AppendDeclarationInputs(RGDeclarationKey& key) const
    {
        key.Add(static_cast<bool>(m_GBuffer));
    }

    void DeferredGPUOcclusionPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);

        // Forward / Forward+ never build a G-Buffer — no-op there (the forward
        // two-phase path is handled by GPUDrivenOcclusionPass instead).
        if (!m_GBuffer)
            return;

        // Phase 2 rebuilds the retained occlusion pyramid IN PLACE from this
        // frame's G-Buffer depth (Renderer3D::BuildCurrentOcclusionHZB). A
        // pass that reads the RETAINED pyramid declares a previous-frame read
        // and is ordered before this one (#1331).
        builder.WriteOutOfBand(RGOutOfBandBoundaries::OcclusionHZB);

        // The phase-2 draws write the G-Buffer, whose attachment views the AO /
        // lighting / SSR consumers read: the shared declaration orders this
        // pass between ScenePass and every one of them. Declared on every
        // deferred frame, not gated on the phase-2 count: the HZB-occlusion
        // toggle flips at runtime without forcing a graph rebuild. Execute
        // no-ops when there is no phase-2 work.
        DeclareGBufferWrites(builder, board);
    }

    void DeferredGPUOcclusionPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // No G-Buffer (Forward path) or no disoccluded set this frame → nothing
        // to do. Drop any queued phase-2 packets (frame-allocator pointers that
        // go stale next frame) even on the bail-out paths.
        if (!m_GBuffer || m_Phase2Packets.IsEmpty())
        {
            m_Phase2Packets.Reset();
            m_Phase2Culls.Reset();
            return;
        }

        GLStateGuard guard("DeferredGPUOcclusionPass", GLStateGuard::Policy::Ignore);

        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // Draw target: per-sample MSAA rasterizes into the multisample G-Buffer
        // FBO (depth still holds occluders + phase-1 there — ScenePass only
        // resolved depth in that mode); otherwise straight into the resolved FBO
        // (ScenePass fully resolved it, so its depth already carries the phase-1
        // survivors). Matches DeferredOpaqueDecalPass's target rule.
        const bool perSampleMSAA = m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1;
        Ref<Framebuffer> targetFB = perSampleMSAA ? m_GBuffer->GetFramebuffer()
                                                  : m_GBuffer->GetSamplingFramebuffer();
        if (!targetFB)
        {
            m_Phase2Packets.Reset();
            m_Phase2Culls.Reset();
            return;
        }

        const RHI::ResourceHandle targetFBID = targetFB->GetRHIHandle();

        // Count the G-Buffer color attachments so the draw-buffer set matches the
        // MRT layout the deferred instanced shader writes (Albedo/Normal/
        // Emissive/Velocity/EntityID).
        const auto& targetSpec = targetFB->GetSpecification();
        u32 colorAttachmentCount = 0;
        for (const auto& att : targetSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++colorAttachmentCount;
        }

        const auto bindGBufferForDraw = [&]()
        {
            targetFB->Bind();
            if (colorAttachmentCount > 0)
            {
                RenderCommand::RestoreAllFramebufferDrawAttachments(targetFBID, colorAttachmentCount);
            }
            context.SetDepthTest(true);
            context.SetDepthMask(true);
            rendererAPI.SetDepthFunc(RHI::CompareOp::Less);
            context.SetBlendState(false);
            rendererAPI.SetCullFace(RHI::CullMode::Back);
            rendererAPI.SetPolygonMode(RHI::PolygonMode::Fill);
            CommandDispatch::BindSceneResources();
        };

        // Phase 2: build a Hi-Z from this frame's resolved G-Buffer depth
        // (occluders + phase-1 survivors laid down by ScenePass), re-test each
        // batch's reject list against it, and draw the disoccluded instances.
        const RHI::ResourceHandle depthTex = m_GBuffer->GetDepthAttachmentHandle();

        // ScenePass wrote (and resolved) this depth via the fixed-function
        // pipeline; the Hi-Z build samples it as a texture. Order the
        // framebuffer-write → texture-fetch explicitly (the forward pass gets
        // the same guarantee from GPUDrivenOcclusionPass::Execute).
        RenderCommand::TextureBarrier();

        const GPUFrustumCuller::HZBOcclusionInputs currentHZB =
            Renderer3D::BuildCurrentOcclusionHZB(depthTex, m_GBuffer->GetWidth(), m_GBuffer->GetHeight());

        if (currentHZB.IsUsable())
        {
            for (const auto& cull : m_Phase2Culls)
                Renderer3D::DispatchOcclusionPhase2(cull, currentHZB);

            bindGBufferForDraw();
            (void)CommandBucket::RecordPackets(rendererAPI, std::span{ m_Phase2Packets.GetData(), static_cast<sizet>(m_Phase2Packets.Num()) });

            targetFB->Unbind();

            // Per-sample MSAA drew into the multisample FBO — resolve so the
            // single-sample views (AO / SSR / lighting) see the phase-2 texels.
            // Non-per-sample drew straight into the resolved FBO, so no resolve
            // is needed. Mirrors DeferredOpaqueDecalPass.
            if (perSampleMSAA)
                m_GBuffer->Resolve();
        }

        // Restore a sane default GL state for whatever runs next.
        context.SetDepthMask(true);
        rendererAPI.SetDepthFunc(RHI::CompareOp::Less);
        context.SetBlendState(false);
        rendererAPI.SetCullFace(RHI::CullMode::Back);
        rendererAPI.SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::BindVertexArrayRaw(RHI::NullResource);
        RenderCommand::BindShaderProgram(RHI::NullResource);

        m_Phase2Packets.Reset();
        m_Phase2Culls.Reset();
    }

    void DeferredGPUOcclusionPass::SubmitPhase2(CommandPacket* packet, const GPUFrustumCuller::TwoPhaseCullResult& cull)
    {
        if (!packet)
            return;
        m_Phase2Packets.Add(packet);
        m_Phase2Culls.Add(cull);
    }

    Ref<Framebuffer> DeferredGPUOcclusionPass::GetTarget() const
    {
        if (!m_GBuffer)
            return nullptr;
        if (m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1)
            return m_GBuffer->GetFramebuffer();
        return m_GBuffer->GetSamplingFramebuffer();
    }

    void DeferredGPUOcclusionPass::OnReset()
    {
        // Drop any queued phase-2 packets so a graph reset / asset reload leaves
        // no dangling frame-allocator pointers.
        m_Phase2Packets.Reset();
        m_Phase2Culls.Reset();
    }
} // namespace OloEngine
