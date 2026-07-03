#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredGPUOcclusionPass.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"

#include <glad/gl.h>

#include <array>

namespace OloEngine
{
    DeferredGPUOcclusionPass::DeferredGPUOcclusionPass()
    {
        SetName("DeferredGPUOcclusionPass");
        OLO_CORE_INFO("Creating DeferredGPUOcclusionPass.");
    }

    void DeferredGPUOcclusionPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedSceneDepthExport = {};
        m_SelectedSceneNormalsExport = {};
        m_SelectedVelocityExport = {};
        m_SelectedGBufferAlbedoExport = {};
        m_SelectedGBufferNormalExport = {};
        m_SelectedGBufferEmissiveExport = {};
        m_SelectedGBufferAlbedoMSExport = {};
        m_SelectedGBufferNormalMSExport = {};
        m_SelectedGBufferEmissiveMSExport = {};
        m_SelectedVelocityMSExport = {};
        m_SelectedSceneDepthMSExport = {};

        // Forward / Forward+ never build a G-Buffer — no-op there (the forward
        // two-phase path is handled by GPUDrivenOcclusionPass instead).
        if (!m_GBuffer)
            return;

        // Re-publish the G-Buffer after our phase-2 draws so the AO / lighting /
        // SSR consumers (which sample the exported textures, not the FBO) see the
        // disoccluded geometry. Declared as TransferDest — the graph orders us
        // between ScenePass (the prior writer) and every downstream reader.
        // Declared UNCONDITIONALLY on every deferred frame (not gated on the
        // per-frame phase-2 count): the HZB-occlusion toggle flips at runtime
        // without forcing a graph rebuild, so gating the writes on the reject
        // count would leave the pass undeclared on the flip frame. Execute
        // no-ops when there is no phase-2 work — a no-op frame passes ScenePass's
        // exported G-Buffer through unchanged.
        const auto declareExport = [&builder](const RGTextureHandle handle, RGTextureHandle& stored)
        {
            if (handle.IsValid())
            {
                stored = handle;
                builder.Write(handle, RGWriteUsage::TransferDest);
            }
        };

        declareExport(board.Scene.SceneDepth, m_SelectedSceneDepthExport);
        declareExport(board.Scene.SceneNormals, m_SelectedSceneNormalsExport);
        declareExport(board.GBuffer.Velocity, m_SelectedVelocityExport);
        declareExport(board.GBuffer.GBufferAlbedo, m_SelectedGBufferAlbedoExport);
        declareExport(board.GBuffer.GBufferNormal, m_SelectedGBufferNormalExport);
        declareExport(board.GBuffer.GBufferEmissive, m_SelectedGBufferEmissiveExport);

        declareExport(board.GBuffer.GBufferAlbedoMS, m_SelectedGBufferAlbedoMSExport);
        declareExport(board.GBuffer.GBufferNormalMS, m_SelectedGBufferNormalMSExport);
        declareExport(board.GBuffer.GBufferEmissiveMS, m_SelectedGBufferEmissiveMSExport);
        declareExport(board.GBuffer.VelocityMS, m_SelectedVelocityMSExport);
        declareExport(board.GBuffer.SceneDepthMS, m_SelectedSceneDepthMSExport);
    }

    void DeferredGPUOcclusionPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // No G-Buffer (Forward path) or no disoccluded set this frame → nothing
        // to do. Drop any queued phase-2 packets (frame-allocator pointers that
        // go stale next frame) even on the bail-out paths.
        if (!m_GBuffer || m_Phase2Packets.empty())
        {
            m_Phase2Packets.clear();
            m_Phase2Culls.clear();
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
            m_Phase2Packets.clear();
            m_Phase2Culls.clear();
            return;
        }

        const u32 targetFBID = targetFB->GetRendererID();

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
                std::array<GLenum, 16> drawBufs{};
                const u32 n = std::min<u32>(colorAttachmentCount, static_cast<u32>(drawBufs.size()));
                for (u32 i = 0; i < n; ++i)
                    drawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
                glNamedFramebufferDrawBuffers(targetFBID, static_cast<GLsizei>(n), drawBufs.data());
            }
            context.SetDepthTest(true);
            context.SetDepthMask(true);
            rendererAPI.SetDepthFunc(GL_LESS);
            context.SetBlendState(false);
            rendererAPI.SetCullFace(GL_BACK);
            rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            CommandDispatch::BindSceneResources();
        };

        // Phase 2: build a Hi-Z from this frame's resolved G-Buffer depth
        // (occluders + phase-1 survivors laid down by ScenePass), re-test each
        // batch's reject list against it, and draw the disoccluded instances.
        const u32 depthTexID = m_GBuffer->GetDepthAttachmentID();

        // ScenePass wrote (and resolved) this depth via the fixed-function
        // pipeline; the Hi-Z build samples it as a texture. Order the
        // framebuffer-write → texture-fetch explicitly (the forward pass gets
        // the same guarantee from GPUDrivenOcclusionPass::Execute).
        ::glTextureBarrier();

        const GPUFrustumCuller::HZBOcclusionInputs currentHZB =
            Renderer3D::BuildCurrentOcclusionHZB(depthTexID, m_GBuffer->GetWidth(), m_GBuffer->GetHeight());

        if (currentHZB.IsUsable())
        {
            for (const auto& cull : m_Phase2Culls)
                Renderer3D::DispatchOcclusionPhase2(cull, currentHZB);

            bindGBufferForDraw();
            for (CommandPacket* packet : m_Phase2Packets)
                if (packet)
                    packet->Execute(rendererAPI);

            targetFB->Unbind();

            // Per-sample MSAA drew into the multisample FBO — resolve so the
            // single-sample export copies below (and AO / SSR) see the phase-2
            // texels. Non-per-sample drew straight into the resolved FBO, so no
            // resolve is needed. Mirrors DeferredOpaqueDecalPass.
            if (perSampleMSAA)
                m_GBuffer->Resolve();

            // Re-export the G-Buffer attachments over ScenePass's earlier copy.
            const u32 width = m_GBuffer->GetWidth();
            const u32 height = m_GBuffer->GetHeight();

            const auto copyExport = [&context, width, height](const RGTextureHandle handle, const u32 sourceTextureID, const GLenum textureTarget)
            {
                if (!handle.IsValid() || sourceTextureID == 0u)
                    return;
                const u32 exportedTextureID = context.ResolveTexture(handle);
                if (exportedTextureID == 0u || exportedTextureID == sourceTextureID)
                    return;
                ::glCopyImageSubData(sourceTextureID, textureTarget, 0, 0, 0, 0,
                                     exportedTextureID, textureTarget, 0, 0, 0, 0,
                                     static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1);
            };

            const u32 albedoID = m_GBuffer->GetColorAttachmentID(GBuffer::Albedo);
            const u32 normalID = m_GBuffer->GetColorAttachmentID(GBuffer::Normal);
            const u32 emissiveID = m_GBuffer->GetColorAttachmentID(GBuffer::Emissive);
            const u32 velocityID = m_GBuffer->GetColorAttachmentID(GBuffer::Velocity);
            const u32 gbufferDepthID = m_GBuffer->GetDepthAttachmentID();

            copyExport(m_SelectedSceneDepthExport, gbufferDepthID, GL_TEXTURE_2D);
            copyExport(m_SelectedSceneNormalsExport, normalID, GL_TEXTURE_2D);
            copyExport(m_SelectedVelocityExport, velocityID, GL_TEXTURE_2D);
            copyExport(m_SelectedGBufferAlbedoExport, albedoID, GL_TEXTURE_2D);
            copyExport(m_SelectedGBufferNormalExport, normalID, GL_TEXTURE_2D);
            copyExport(m_SelectedGBufferEmissiveExport, emissiveID, GL_TEXTURE_2D);

            // Only re-export the multisample attachments when phase-2 actually
            // rasterized into them (per-sample MSAA path). Non-per-sample mode
            // drew straight into the resolved FBO and left the MS attachments
            // untouched, so the MS exports already carry ScenePass's data and a
            // re-copy here would be redundant. Matches the perSampleMSAA-gated
            // Resolve() above.
            if (perSampleMSAA)
            {
                copyExport(m_SelectedGBufferAlbedoMSExport, m_GBuffer->GetMSColorAttachmentID(GBuffer::Albedo), GL_TEXTURE_2D_MULTISAMPLE);
                copyExport(m_SelectedGBufferNormalMSExport, m_GBuffer->GetMSColorAttachmentID(GBuffer::Normal), GL_TEXTURE_2D_MULTISAMPLE);
                copyExport(m_SelectedGBufferEmissiveMSExport, m_GBuffer->GetMSColorAttachmentID(GBuffer::Emissive), GL_TEXTURE_2D_MULTISAMPLE);
                copyExport(m_SelectedVelocityMSExport, m_GBuffer->GetMSColorAttachmentID(GBuffer::Velocity), GL_TEXTURE_2D_MULTISAMPLE);
                copyExport(m_SelectedSceneDepthMSExport, m_GBuffer->GetMSDepthAttachmentID(), GL_TEXTURE_2D_MULTISAMPLE);
            }
        }

        // Restore a sane default GL state for whatever runs next.
        context.SetDepthMask(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        context.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        ::glBindVertexArray(0);
        ::glUseProgram(0);

        m_Phase2Packets.clear();
        m_Phase2Culls.clear();
    }

    void DeferredGPUOcclusionPass::SubmitPhase2(CommandPacket* packet, const GPUFrustumCuller::TwoPhaseCullResult& cull)
    {
        if (!packet)
            return;
        m_Phase2Packets.push_back(packet);
        m_Phase2Culls.push_back(cull);
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
        m_Phase2Packets.clear();
        m_Phase2Culls.clear();
    }
} // namespace OloEngine
