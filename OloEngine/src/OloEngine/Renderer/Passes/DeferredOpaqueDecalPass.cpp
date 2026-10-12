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
            }
            [[maybe_unused]] const auto depth = builder.Read(board.Scene.SceneDepth, RGReadUsage::ShaderSample);
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

    void DeferredOpaqueDecalPass::Execute(RGCommandContext& /*context*/)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_GBuffer)
            return;

        const bool hasDecalWork = m_DecalPass && m_DecalPass->HasSubmittedCommands();

        // Mirror the original synchronous call that used to live inline in
        // SceneRenderPass::Execute(). The MSAA per-sample path writes into
        // the multisample FBO but samples resolved depth; non-per-sample
        // paths operate entirely on the resolved FBO.
        if (hasDecalWork && m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1)
        {
            m_GBuffer->ResolveDepthOnly();
            RenderCommand::TextureBarrier();
            m_DecalPass->ExecuteOnGBuffer(m_GBuffer->GetFramebuffer(),
                                          m_GBuffer->GetSamplingFramebuffer());
        }
        else if (hasDecalWork)
        {
            m_DecalPass->ExecuteOnGBuffer(m_GBuffer->GetSamplingFramebuffer());
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
