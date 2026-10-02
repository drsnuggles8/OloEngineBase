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
        key.Add(m_DecalPass && m_DecalPass->HasSubmittedCommands());
    }

    void DeferredOpaqueDecalPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneNormalsExport = {};
        m_SelectedGBufferAlbedoExport = {};
        m_SelectedGBufferNormalExport = {};
        m_SelectedGBufferEmissiveExport = {};

        if (!m_GBuffer)
            return;

        const bool hasDecalWork = m_DecalPass && m_DecalPass->HasSubmittedCommands();

        // The decal shader reconstructs world position from the scene depth, so
        // this pass samples the DEPTH attachment while writing the COLOUR ones.
        const bool samplesSceneDepth = hasDecalWork && blackboard.Scene.SceneDepth.IsValid();
        if (samplesSceneDepth)
        {
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scene.SceneNormals.IsValid())
        {
            m_SelectedSceneNormalsExport = blackboard.Scene.SceneNormals;
            builder.Write(blackboard.Scene.SceneNormals, RGWriteUsage::TransferDest);
        }

        if (blackboard.GBuffer.GBufferAlbedo.IsValid())
        {
            m_SelectedGBufferAlbedoExport = blackboard.GBuffer.GBufferAlbedo;
            builder.Write(blackboard.GBuffer.GBufferAlbedo, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBuffer.GBufferNormal.IsValid())
        {
            m_SelectedGBufferNormalExport = blackboard.GBuffer.GBufferNormal;
            builder.Write(blackboard.GBuffer.GBufferNormal, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBuffer.GBufferEmissive.IsValid())
        {
            m_SelectedGBufferEmissiveExport = blackboard.GBuffer.GBufferEmissive;
            builder.Write(blackboard.GBuffer.GBufferEmissive, RGWriteUsage::TransferDest);
        }

        // SceneDepth, SceneNormals and the three GBuffer* views are all
        // attachment views of the SAME framebuffer (RenderPipeline.cpp's
        // resolvedGBuffer). Read() propagates to the parent framebuffer and the
        // validator then expands that parent read back down onto every sibling
        // attachment view — so the depth sample above reads, by name, every
        // colour view this pass writes, and each one is reported as a same-pass
        // feedback hazard. The subresources never actually overlap: one depth
        // attachment read, four colour attachments written. This is the exact
        // legitimate-RMW case RGBuilder::Write's comment names, and
        // AllowSamePassReadWrite is how a pass states it.
        //
        // Not cosmetic: OLO_CORE_ASSERT on the compiled-hazard list is a
        // __debugbreak, so in a Debug build without a debugger this KILLED the
        // editor on the first frame after opening any Deferred scene containing
        // an opaque decal. It went unnoticed because no sandbox scene put a
        // decal on the deferred path until DecalModeMatrixTest.olo — the graph
        // validation only runs when the graph's shape changes, and the decal
        // pass only declares these accesses when it has decal work.
        if (samplesSceneDepth)
        {
            for (const auto& written : { m_SelectedSceneNormalsExport, m_SelectedGBufferAlbedoExport,
                                         m_SelectedGBufferNormalExport, m_SelectedGBufferEmissiveExport })
            {
                if (written.IsValid())
                    builder.AllowSamePassReadWrite(written);
            }
        }

        if (blackboard.GBuffer.GBufferAlbedoMS.IsValid())
            builder.Write(blackboard.GBuffer.GBufferAlbedoMS, RGWriteUsage::TransferDest);
        if (blackboard.GBuffer.GBufferNormalMS.IsValid())
            builder.Write(blackboard.GBuffer.GBufferNormalMS, RGWriteUsage::TransferDest);
        if (blackboard.GBuffer.GBufferEmissiveMS.IsValid())
            builder.Write(blackboard.GBuffer.GBufferEmissiveMS, RGWriteUsage::TransferDest);
        if (blackboard.GBuffer.VelocityMS.IsValid())
            builder.Write(blackboard.GBuffer.VelocityMS, RGWriteUsage::TransferDest);
        if (blackboard.GBuffer.SceneDepthMS.IsValid())
            builder.Write(blackboard.GBuffer.SceneDepthMS, RGWriteUsage::TransferDest);
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

        // Decals drawn into the multisample G-Buffer (per-sample lighting) are
        // resolved so the single-sample views the AO / lighting / SSR readers
        // sample carry them. Nothing is copied: the G-Buffer exports are views
        // of these attachments, and the re-export this pass used to run
        // compared each view with its own attachment and never copied a texel
        // (#1332's copy ledger).
        if (hasDecalWork && m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1u)
            m_GBuffer->Resolve();
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
