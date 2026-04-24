#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DeferredOpaqueDecalPass.h"

#include "OloEngine/Renderer/Passes/DecalRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    DeferredOpaqueDecalPass::DeferredOpaqueDecalPass()
    {
        SetName("DeferredOpaqueDecalPass");
    }

    void DeferredOpaqueDecalPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Decals composite into the G-Buffer albedo/normal/emissive RTs.
        // Those RTs are aliased to the shared scene framebuffer in deferred
        // mode (SceneRenderPass writes SceneColor/SceneDepth), so declare
        // the same names here for the hazard validator. This makes the
        // implicit "must run after ScenePass, must run before
        // DeferredLightingPass" ordering visible to the graph.
        DeclareRead(ResourceNames::SceneDepth, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
    }

    void DeferredOpaqueDecalPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_DecalPass || !m_GBuffer)
            return;

        // Mirror the original synchronous call that used to live inline in
        // SceneRenderPass::Execute(). The MSAA per-sample path writes into
        // the multisample FBO but samples resolved depth; non-per-sample
        // paths operate entirely on the resolved FBO.
        if (m_PerSampleLighting && m_GBuffer->GetSampleCount() > 1)
        {
            m_DecalPass->ExecuteOnGBuffer(m_GBuffer->GetFramebuffer(),
                                          m_GBuffer->GetSamplingFramebuffer());
        }
        else
        {
            m_DecalPass->ExecuteOnGBuffer(m_GBuffer->GetSamplingFramebuffer());
        }
    }

    Ref<Framebuffer> DeferredOpaqueDecalPass::GetTarget() const
    {
        // No owned framebuffer — decals rasterize into the GBuffer FBO via
        // DecalRenderPass::ExecuteOnGBuffer. Expose the write target so
        // graph consumers see something meaningful.
        if (m_GBuffer)
            return m_GBuffer->GetSamplingFramebuffer();
        return nullptr;
    }
} // namespace OloEngine
