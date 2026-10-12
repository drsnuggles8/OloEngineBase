#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.h"

#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    DeferredOpaqueDecalPass::DeferredOpaqueDecalPass()
    {
        SetName("DeferredOpaqueDecalPass");
    }

    void DeferredOpaqueDecalPass::AppendDeclarationInputs(RGDeclarationKey& key) const
    {
        key.Add(static_cast<bool>(m_GBuffer));
        key.Add(m_PerSampleLighting);
        key.Add(m_DecalPass && m_DecalPass->HasSubmittedCommands());
    }

    void DeferredOpaqueDecalPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedSceneNormalsExport = {};
        m_SelectedGBufferAlbedoExport = {};
        m_SelectedGBufferNormalExport = {};
        m_SelectedGBufferEmissiveExport = {};
        m_ProjectionDepth = {};
        if (!m_GBuffer)
            return;
        const bool multisample = m_PerSampleLighting && board.Config.GBufferSamples > 1u;
        const bool hasDecalWork = m_DecalPass && m_DecalPass->HasSubmittedCommands();
        if (hasDecalWork && board.Scene.SceneDepth.IsValid())
        {
            if (multisample)
            {
                [[maybe_unused]] const auto source = builder.ReadTransferSourceBeforeWrite(board.GBuffer.SceneDepthMS);
                builder.Write(board.Scene.SceneDepth, RGWriteUsage::TransferDest);
                // Execute transfers first, then samples this independent backing.
                builder.AllowSamePassReadWrite(board.Scene.SceneDepth);
                m_ProjectionDepth = board.Scene.SceneDepth;
            }
            else
            {
                auto desc = RGResourceDesc::FromHandleKind(RGResourceHandle::Kind::Framebuffer, "OpaqueDecalProjectionDepthBacking");
                desc.Format = RGResourceFormat::Depth24Stencil8;
                desc.Attachments = { RGResourceFormat::Depth24Stencil8 };
                desc.Width = board.Config.GBufferWidth;
                desc.Height = board.Config.GBufferHeight;
                const auto backing = builder.CreateFramebuffer("OpaqueDecalProjectionDepthBacking", desc);
                // Attachment storage preserves the source's clamp/linear sampler
                // contract, unlike a bare transient texture's repeat defaults.
                m_ProjectionDepth = builder.CreateFramebufferDepthAttachmentView("OpaqueDecalProjectionDepth", backing);
                [[maybe_unused]] const auto source = builder.ReadTransferSourceBeforeWrite(board.Scene.SceneDepth);
                builder.Write(m_ProjectionDepth, RGWriteUsage::TransferDest);
                builder.AllowSamePassReadWrite(m_ProjectionDepth);
            }
            [[maybe_unused]] const auto depth = builder.Read(m_ProjectionDepth, RGReadUsage::ShaderSample);
        }
        m_SelectedSceneNormalsExport = multisample ? board.GBuffer.GBufferNormalMS : board.Scene.SceneNormals;
        m_SelectedGBufferAlbedoExport = multisample ? board.GBuffer.GBufferAlbedoMS : board.GBuffer.GBufferAlbedo;
        m_SelectedGBufferNormalExport = multisample ? board.GBuffer.GBufferNormalMS : board.GBuffer.GBufferNormal;
        m_SelectedGBufferEmissiveExport = multisample ? board.GBuffer.GBufferEmissiveMS : board.GBuffer.GBufferEmissive;
        for (const auto written : { m_SelectedSceneNormalsExport, m_SelectedGBufferAlbedoExport,
                                    m_SelectedGBufferNormalExport, m_SelectedGBufferEmissiveExport })
        {
            if (written.IsValid())
            {
                [[maybe_unused]] const auto previous = builder.Read(written, RGReadUsage::RenderTargetRead);
                builder.Write(written, RGWriteUsage::RenderTarget);
            }
        }
        const auto rasterDepth = multisample ? board.GBuffer.SceneDepthMS : board.Scene.SceneDepth;
        if (rasterDepth.IsValid())
        {
            [[maybe_unused]] const auto depthTest = builder.Read(rasterDepth, RGReadUsage::RenderTargetRead);
        }
    }

    void DeferredOpaqueDecalPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_GBuffer)
            return;

        const bool hasDecalWork = m_DecalPass && m_DecalPass->HasSubmittedCommands();

        // Mirror the original synchronous call that used to live inline in
        // SceneRenderPass::Execute(). The MSAA per-sample path writes into
        // the multisample FBO but samples resolved depth; non-per-sample
        // paths snapshot depth before rasterizing into the resolved FBO.
        if (hasDecalWork && m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1)
        {
            m_GBuffer->ResolveDepthOnly();
            RenderCommand::TextureBarrier();
            m_DecalPass->ExecuteOnGBuffer(m_GBuffer->GetFramebuffer(),
                                          m_GBuffer->GetDepthAttachmentHandle());
        }
        else if (hasDecalWork)
        {
            const auto projectionDepth = context.ResolveTextureHandle(m_ProjectionDepth);
            if (!projectionDepth.IsValid())
                return;
            RenderCommand::CopyImageSubData(m_GBuffer->GetDepthAttachmentHandle(), RendererAPI::TextureTargetType::Texture2D,
                                            projectionDepth, RendererAPI::TextureTargetType::Texture2D,
                                            m_GBuffer->GetWidth(), m_GBuffer->GetHeight());
            RenderCommand::TextureBarrier();
            m_DecalPass->ExecuteOnGBuffer(m_GBuffer->GetSamplingFramebuffer(), projectionDepth);
        }
        else
        {
            // No additional handling required.
        }

        // The final graph resolve publishes multisample decal writes.
    }

    Ref<Framebuffer> DeferredOpaqueDecalPass::GetTarget() const
    {
        // No owned framebuffer — decals rasterize into the GBuffer FBO via
        // DecalRenderPass::ExecuteOnGBuffer. Expose the actual write target
        // so graph consumers (and the hazard validator) see the FB this
        // pass mutates. In MSAA per-sample mode decals are broadcast into
        // the multisample FB (samples resolved depth from the single-
        // sample FB); otherwise writes go directly into the resolved FB.
        if (!m_GBuffer)
            return nullptr;
        if (m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1)
            return m_GBuffer->GetFramebuffer();
        return m_GBuffer->GetSamplingFramebuffer();
    }
} // namespace OloEngine
