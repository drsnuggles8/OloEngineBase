#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ScenePrepassRenderPass.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"

namespace OloEngine
{
    ScenePrepassRenderPass::ScenePrepassRenderPass(SceneRenderPass* scene)
        : m_Scene(scene)
    {
        SetName("ScenePrepassPass");
    }

    void ScenePrepassRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        OLO_CORE_ASSERT(m_Scene, "ScenePrepassRenderPass needs the ScenePass whose bucket it renders");
        if (blackboard.Scene.SceneColor.IsValid())
            SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
        m_Scene->SetupForwardPrepass(builder, blackboard);
    }

    void ScenePrepassRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Scene)
            return;
        Ref<Framebuffer> sceneTarget;
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
            sceneTarget = context.ResolveFramebuffer(sceneHandle);
        m_Scene->ExecuteForwardPrepass(context, sceneTarget);
    }
} // namespace OloEngine
