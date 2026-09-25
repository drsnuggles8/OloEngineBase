#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GPUDrivenOcclusionPrepassPass.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Passes/GPUDrivenOcclusionPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"

namespace OloEngine
{
    GPUDrivenOcclusionPrepassPass::GPUDrivenOcclusionPrepassPass(GPUDrivenOcclusionPass* occlusion)
        : m_Occlusion(occlusion)
    {
        SetName("GPUDrivenOcclusionPrepassPass");
    }

    void GPUDrivenOcclusionPrepassPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        OLO_CORE_ASSERT(m_Occlusion, "GPUDrivenOcclusionPrepassPass needs the pass whose batches it renders");
        if (blackboard.Scene.SceneColor.IsValid())
            SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
        m_Occlusion->SetupForwardPrepass(builder, blackboard);
    }

    void GPUDrivenOcclusionPrepassPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Occlusion)
            return;
        Ref<Framebuffer> sceneTarget;
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
            sceneTarget = context.ResolveFramebuffer(sceneHandle);
        m_Occlusion->ExecuteForwardPrepass(context, sceneTarget);
    }
} // namespace OloEngine
