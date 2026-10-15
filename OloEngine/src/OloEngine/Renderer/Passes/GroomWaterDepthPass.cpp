#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GroomWaterDepthPass.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"

namespace OloEngine
{
    GroomWaterDepthPass::GroomWaterDepthPass(WaterRenderPass* water) : m_Water(water)
    {
        SetName("GroomWaterDepthPass");
    }

    void GroomWaterDepthPass::AppendDeclarationInputs(RGDeclarationKey& key) const
    {
        key.Add(m_Water && m_Water->HasSubmittedCommands());
    }

    void GroomWaterDepthPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_Target = {};
        m_Scene = board.Scene.SceneColor;
        if (!m_Water || !m_Water->HasSubmittedCommands() || !board.Scene.GroomWaterSurface.IsValid())
            return;
        m_Target = board.Scene.GroomWaterSurface;
        builder.Write(board.Scene.GroomWaterSurfaceDepth, RGWriteUsage::RenderTarget);
    }

    void GroomWaterDepthPass::Execute(RGCommandContext& context)
    {
        if (!m_Target.IsValid() || !m_Water)
            return;
        const auto target = context.ResolveFramebuffer(m_Target);
        const auto scene = context.ResolveFramebuffer(m_Scene);
        if (target && scene)
            m_Water->CaptureSurfaceDepth(context, target, scene->GetActiveViewportWidth(), scene->GetActiveViewportHeight());
    }
} // namespace OloEngine
