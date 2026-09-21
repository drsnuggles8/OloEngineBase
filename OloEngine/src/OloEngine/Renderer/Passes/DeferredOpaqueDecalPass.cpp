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

    void DeferredOpaqueDecalPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneNormalsExport = {};
        m_SelectedGBufferAlbedoExport = {};
        m_SelectedGBufferNormalExport = {};
        m_SelectedGBufferEmissiveExport = {};
        m_SelectedGBufferAlbedoMSExport = {};
        m_SelectedGBufferNormalMSExport = {};
        m_SelectedGBufferEmissiveMSExport = {};
        m_SelectedVelocityMSExport = {};
        m_SelectedSceneDepthMSExport = {};

        if (!m_GBuffer)
            return;

        const bool hasDecalWork = m_DecalPass && m_DecalPass->GetCommandBucket().GetCommandCount() > 0;

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
        {
            m_SelectedGBufferAlbedoMSExport = blackboard.GBuffer.GBufferAlbedoMS;
            builder.Write(blackboard.GBuffer.GBufferAlbedoMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBuffer.GBufferNormalMS.IsValid())
        {
            m_SelectedGBufferNormalMSExport = blackboard.GBuffer.GBufferNormalMS;
            builder.Write(blackboard.GBuffer.GBufferNormalMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBuffer.GBufferEmissiveMS.IsValid())
        {
            m_SelectedGBufferEmissiveMSExport = blackboard.GBuffer.GBufferEmissiveMS;
            builder.Write(blackboard.GBuffer.GBufferEmissiveMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBuffer.VelocityMS.IsValid())
        {
            m_SelectedVelocityMSExport = blackboard.GBuffer.VelocityMS;
            builder.Write(blackboard.GBuffer.VelocityMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBuffer.SceneDepthMS.IsValid())
        {
            m_SelectedSceneDepthMSExport = blackboard.GBuffer.SceneDepthMS;
            builder.Write(blackboard.GBuffer.SceneDepthMS, RGWriteUsage::TransferDest);
        }
    }

    void DeferredOpaqueDecalPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_GBuffer)
            return;

        const bool hasDecalWork = m_DecalPass && m_DecalPass->GetCommandBucket().GetCommandCount() > 0;

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

        const bool anySingleSampleExportRequested = m_SelectedSceneNormalsExport.IsValid() ||
                                                    m_SelectedGBufferAlbedoExport.IsValid() ||
                                                    m_SelectedGBufferNormalExport.IsValid() ||
                                                    m_SelectedGBufferEmissiveExport.IsValid();
        const bool anyMultisampleExportRequested = m_SelectedGBufferAlbedoMSExport.IsValid() ||
                                                   m_SelectedGBufferNormalMSExport.IsValid() ||
                                                   m_SelectedGBufferEmissiveMSExport.IsValid() ||
                                                   m_SelectedVelocityMSExport.IsValid() ||
                                                   m_SelectedSceneDepthMSExport.IsValid();
        if (!anySingleSampleExportRequested && !anyMultisampleExportRequested)
            return;

        if (anySingleSampleExportRequested && hasDecalWork && m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1u)
        {
            m_GBuffer->Resolve();
        }

        const auto copyGBufferExport = [this, &context](const RGTextureHandle handle, const RHI::ResourceHandle sourceTextureID)
        {
            if (!handle.IsValid() || !sourceTextureID.IsValid())
                return;

            const RHI::ResourceHandle exportedTextureID = context.ResolveTextureHandle(handle);
            if (!exportedTextureID.IsValid() || exportedTextureID == sourceTextureID)
                return;

            RenderCommand::CopyImageSubData(sourceTextureID, RendererAPI::TextureTargetType::Texture2D,
                                            exportedTextureID, RendererAPI::TextureTargetType::Texture2D,
                                            m_GBuffer->GetWidth(), m_GBuffer->GetHeight());
        };

        const auto copyMultisampleGBufferExport = [this, &context](const RGTextureHandle handle, const RHI::ResourceHandle sourceTextureID)
        {
            if (!handle.IsValid() || !sourceTextureID.IsValid())
                return;

            const RHI::ResourceHandle exportedTextureID = context.ResolveTextureHandle(handle);
            if (!exportedTextureID.IsValid() || exportedTextureID == sourceTextureID)
                return;

            RenderCommand::CopyImageSubData(sourceTextureID, RendererAPI::TextureTargetType::Texture2DMultisample,
                                            exportedTextureID, RendererAPI::TextureTargetType::Texture2DMultisample,
                                            m_GBuffer->GetWidth(), m_GBuffer->GetHeight());
        };

        const RHI::ResourceHandle albedoID = m_GBuffer->GetColorAttachmentHandle(GBuffer::Albedo);
        const RHI::ResourceHandle normalID = m_GBuffer->GetColorAttachmentHandle(GBuffer::Normal);
        const RHI::ResourceHandle emissiveID = m_GBuffer->GetColorAttachmentHandle(GBuffer::Emissive);

        const RHI::ResourceHandle albedoMSID = m_GBuffer->GetMSColorAttachmentHandle(GBuffer::Albedo);
        const RHI::ResourceHandle normalMSID = m_GBuffer->GetMSColorAttachmentHandle(GBuffer::Normal);
        const RHI::ResourceHandle emissiveMSID = m_GBuffer->GetMSColorAttachmentHandle(GBuffer::Emissive);
        const RHI::ResourceHandle velocityMSID = m_GBuffer->GetMSColorAttachmentHandle(GBuffer::Velocity);
        const RHI::ResourceHandle depthMSID = m_GBuffer->GetMSDepthAttachmentHandle();

        copyGBufferExport(m_SelectedSceneNormalsExport, normalID);
        copyGBufferExport(m_SelectedGBufferAlbedoExport, albedoID);
        copyGBufferExport(m_SelectedGBufferNormalExport, normalID);
        copyGBufferExport(m_SelectedGBufferEmissiveExport, emissiveID);

        if (m_GBuffer->GetSampleCount() > 1u)
        {
            copyMultisampleGBufferExport(m_SelectedGBufferAlbedoMSExport, albedoMSID);
            copyMultisampleGBufferExport(m_SelectedGBufferNormalMSExport, normalMSID);
            copyMultisampleGBufferExport(m_SelectedGBufferEmissiveMSExport, emissiveMSID);
            copyMultisampleGBufferExport(m_SelectedVelocityMSExport, velocityMSID);
            copyMultisampleGBufferExport(m_SelectedSceneDepthMSExport, depthMSID);
        }
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
