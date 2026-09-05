#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/VirtualShadowMapMarkPass.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace OloEngine
{
    VirtualShadowMapMarkPass::VirtualShadowMapMarkPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("VirtualShadowMapMarkPass");
        SetPassWorkType(PassWorkType::Compute);
        SetAsyncComputeCandidate(true);
        // NeverCull, because this pass's only output is an SSBO the graph does
        // not model: it writes the VSM page table, which the NEXT frame's shadow
        // pass consumes. To reachability analysis that looks like a node whose
        // results nobody reads, so it is pruned — and the symptom is a completely
        // unshadowed frame with no error anywhere, because nothing is ever marked,
        // so nothing is ever allocated. Same flag, same reason, as
        // DDGIProbeUpdatePass and FluidIntermediatesPass.
        SetSideEffects(SideEffect::NeverCull);
    }

    bool VirtualShadowMapMarkPass::IsEnabled() const noexcept
    {
        // See the header: the graph caches its topology, so this must not depend
        // on whether VSM happens to be on right now.
        return m_ShadowMap != nullptr;
    }

    bool VirtualShadowMapMarkPass::VirtualShadowMapActive() const noexcept
    {
        return m_ShadowMap != nullptr && m_ShadowMap->IsVirtualShadowMapActive();
    }

    void VirtualShadowMapMarkPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        m_SceneDepth = {};

        // DELIBERATELY NOT GATED ON VirtualShadowMapActive(), for the same reason
        // IsEnabled() is not: RenderGraph::BuildFrameGraph caches the compiled
        // frame behind a fingerprint, and that fingerprint does not include
        // whether VSM is on. So Setup runs on the frame the topology was last
        // rebuilt and NOT when the setting is toggled — an early return here
        // leaves m_SceneDepth invalid forever, Execute() bails on it every frame,
        // no page is ever marked, nothing is ever allocated, and the frame renders
        // completely unshadowed with no error anywhere.
        //
        // That is not hypothetical: it is why enabling VSM produced a correct
        // shadow when it was on before the first frame, and NO shadow when it was
        // switched on after some frames had already been drawn — which is the
        // order the editor's toggle, and the CSM-then-VSM comparison test, both
        // use. Declaring a read the pass may not use costs one graph edge; this
        // node is NeverCull, so it does not even change what gets culled.
        //
        // Read the SEMANTIC scene depth, not an attachment view: the pass has to
        // work on the forward path (a snapshot texture) and the deferred one (a
        // G-Buffer attachment view / MSAA resolve) alike, and the blackboard's
        // SceneDepth is the handle that means the same thing on both.
        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SceneDepth = blackboard.Scene.SceneDepth;
            builder.Read(m_SceneDepth);
        }
    }

    void VirtualShadowMapMarkPass::Init(const FramebufferSpecification& spec)
    {
        m_FramebufferSpec = spec;
    }

    void VirtualShadowMapMarkPass::Execute(RGCommandContext& context)
    {
        auto prepared = PrepareParallelRecording(context);
        if (prepared.Record)
            prepared.Record(context);
        if (prepared.Publish)
            prepared.Publish();
    }

    RGPreparedPass VirtualShadowMapMarkPass::PrepareParallelRecording(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!VirtualShadowMapActive() || !m_SceneDepth.IsValid())
            return {};

        const RHI::ResourceHandle sceneDepth = context.ResolveTextureHandle(m_SceneDepth);
        if (!sceneDepth.IsValid())
            return {};

        u32 width = 0;
        u32 height = 0;
        RenderCommand::GetTextureDimensions(sceneDepth, 0, width, height);
        if (width == 0 || height == 0)
            return {};

        // Depth NDC -> render-relative world. The RECONSTRUCTION flavour of the
        // projection, not the rasterizer one: this shader does its own
        // `ndc * 2 - 1` unprojection, and the rasterizer flavour would
        // double-apply the Vulkan z remap (ADR 0011 (59) — a seam is defined by
        // how a value is READ). Identity on GL, which is the only backend the VSM
        // runs on today, but naming the right one keeps the port honest.
        //
        // (Both backends run the VSM now; the note stands because the flavour is
        // chosen by how the value is READ, not by which backend is up.)
        const glm::mat4& view = Renderer3D::GetViewMatrix();
        const glm::mat4 projection = RHI::AdjustProjectionForShaderReconstruction(Renderer3D::GetProjectionMatrix());
        const glm::mat4 inverseViewProjection = glm::inverse(projection * view);

        // The camera position in the SAME space the clip projections live in.
        // s_Data.ViewMatrix is the ABSOLUTE view (Scene builds it from the camera
        // transform, not the render-relative one), so the position comes out of
        // its inverse in absolute world space and has to be shifted by the render
        // origin — exactly what ShadowMap does for the CSM path. Getting this
        // wrong shifts the clip levels' concentric rings relative to the ones the
        // lit pass samples, which reads as a clip-level seam rather than as a
        // wrong camera position (see agent-rules/virtual-shadow-map-page-cache.md
        // §1).
        const glm::vec3 cameraWorld = glm::vec3(glm::inverse(view)[3]);
        const glm::vec3 cameraRelative = cameraWorld - Renderer3D::GetRenderOrigin();

        return m_ShadowMap->GetVirtualShadowMap().PreparePageMarking(sceneDepth, width, height,
                                                                     inverseViewProjection, cameraRelative);
    }

    Ref<Framebuffer> VirtualShadowMapMarkPass::GetTarget() const
    {
        // Compute-only: the pass writes an SSBO, so it owns no render target and
        // must not appear in the SceneColor chain.
        return nullptr;
    }

    void VirtualShadowMapMarkPass::SetupFramebuffer(u32 width, u32 height)
    {
        ResizeFramebuffer(width, height);
    }

    void VirtualShadowMapMarkPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }
} // namespace OloEngine
