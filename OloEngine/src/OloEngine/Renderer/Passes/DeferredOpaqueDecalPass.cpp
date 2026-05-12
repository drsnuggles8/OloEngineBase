#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.h"

#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

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

        if (hasDecalWork && blackboard.SceneDepth.IsValid())
        {
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.SceneDepth, RGReadUsage::ShaderSample);
        }

        if (blackboard.SceneNormals.IsValid())
        {
            m_SelectedSceneNormalsExport = blackboard.SceneNormals;
            builder.Write(blackboard.SceneNormals, RGWriteUsage::TransferDest);
        }

        if (blackboard.GBufferAlbedo.IsValid())
        {
            m_SelectedGBufferAlbedoExport = blackboard.GBufferAlbedo;
            builder.Write(blackboard.GBufferAlbedo, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBufferNormal.IsValid())
        {
            m_SelectedGBufferNormalExport = blackboard.GBufferNormal;
            builder.Write(blackboard.GBufferNormal, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBufferEmissive.IsValid())
        {
            m_SelectedGBufferEmissiveExport = blackboard.GBufferEmissive;
            builder.Write(blackboard.GBufferEmissive, RGWriteUsage::TransferDest);
        }

        if (blackboard.GBufferAlbedoMS.IsValid())
        {
            m_SelectedGBufferAlbedoMSExport = blackboard.GBufferAlbedoMS;
            builder.Write(blackboard.GBufferAlbedoMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBufferNormalMS.IsValid())
        {
            m_SelectedGBufferNormalMSExport = blackboard.GBufferNormalMS;
            builder.Write(blackboard.GBufferNormalMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.GBufferEmissiveMS.IsValid())
        {
            m_SelectedGBufferEmissiveMSExport = blackboard.GBufferEmissiveMS;
            builder.Write(blackboard.GBufferEmissiveMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.VelocityMS.IsValid())
        {
            m_SelectedVelocityMSExport = blackboard.VelocityMS;
            builder.Write(blackboard.VelocityMS, RGWriteUsage::TransferDest);
        }
        if (blackboard.SceneDepthMS.IsValid())
        {
            m_SelectedSceneDepthMSExport = blackboard.SceneDepthMS;
            builder.Write(blackboard.SceneDepthMS, RGWriteUsage::TransferDest);
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

        const auto copyGBufferExport = [&](const RGTextureHandle handle, const u32 sourceTextureID)
        {
            if (!handle.IsValid() || sourceTextureID == 0u)
                return;

            const u32 exportedTextureID = context.ResolveTexture(handle);
            if (exportedTextureID == 0u || exportedTextureID == sourceTextureID)
                return;

            glCopyImageSubData(sourceTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               exportedTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                               static_cast<GLsizei>(m_GBuffer->GetWidth()),
                               static_cast<GLsizei>(m_GBuffer->GetHeight()),
                               1);
        };

        const auto copyMultisampleGBufferExport = [&](const RGTextureHandle handle, const u32 sourceTextureID)
        {
            if (!handle.IsValid() || sourceTextureID == 0u)
                return;

            const u32 exportedTextureID = context.ResolveTexture(handle);
            if (exportedTextureID == 0u || exportedTextureID == sourceTextureID)
                return;

            glCopyImageSubData(sourceTextureID, GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0,
                               exportedTextureID, GL_TEXTURE_2D_MULTISAMPLE, 0, 0, 0, 0,
                               static_cast<GLsizei>(m_GBuffer->GetWidth()),
                               static_cast<GLsizei>(m_GBuffer->GetHeight()),
                               1);
        };

        const u32 albedoID = m_GBuffer->GetColorAttachmentID(GBuffer::Albedo);
        const u32 normalID = m_GBuffer->GetColorAttachmentID(GBuffer::Normal);
        const u32 emissiveID = m_GBuffer->GetColorAttachmentID(GBuffer::Emissive);

        const u32 albedoMSID = m_GBuffer->GetMSColorAttachmentID(GBuffer::Albedo);
        const u32 normalMSID = m_GBuffer->GetMSColorAttachmentID(GBuffer::Normal);
        const u32 emissiveMSID = m_GBuffer->GetMSColorAttachmentID(GBuffer::Emissive);
        const u32 velocityMSID = m_GBuffer->GetMSColorAttachmentID(GBuffer::Velocity);
        const u32 depthMSID = m_GBuffer->GetMSDepthAttachmentID();

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
