#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FoliagePrepassPass.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Passes/FoliageRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"

namespace OloEngine
{
    FoliagePrepassPass::FoliagePrepassPass(FoliageRenderPass* foliage)
        : m_Foliage(foliage)
    {
        SetName("FoliagePrepassPass");
    }

    void FoliagePrepassPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        OLO_CORE_ASSERT(m_Foliage, "FoliagePrepassPass needs the pass whose bucket it renders");
        if (blackboard.Scene.SceneColor.IsValid())
            SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
        m_Foliage->SetupForwardPrepass(builder, blackboard);
    }

    void FoliagePrepassPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Foliage)
            return;
        Ref<Framebuffer> sceneTarget;
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
            sceneTarget = context.ResolveFramebuffer(sceneHandle);
        m_Foliage->ExecuteForwardPrepass(context, sceneTarget);
    }
} // namespace OloEngine
