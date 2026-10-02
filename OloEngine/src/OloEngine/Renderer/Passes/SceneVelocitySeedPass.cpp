#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SceneVelocitySeedPass.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    SceneVelocitySeedPass::SceneVelocitySeedPass(SceneRenderPass* scene)
        : m_Scene(scene)
    {
        SetName("SceneVelocitySeedPass");
    }

    void SceneVelocitySeedPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_Source = {};
        m_Destination = {};

        const RGTextureHandle source = blackboard.GBuffer.Velocity;
        const RGTextureHandle destination = blackboard.Scene.SceneVelocity;
        // The same handle is the forward paths: SceneColor RT3 already is the
        // velocity every writer wrote, and there is nothing to seed.
        if (!source.IsValid() || !destination.IsValid() || source == destination)
            return;

        m_Source = source;
        m_Destination = destination;
        [[maybe_unused]] const auto sourceRead = builder.Read(source, RGReadUsage::TransferSource);
        builder.Write(destination, RGWriteUsage::TransferDest);
    }

    void SceneVelocitySeedPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Scene || !m_Source.IsValid() || !m_Destination.IsValid())
            return;

        // The G-Buffer and SceneColor are both allocated at the scene's render
        // extent, the size the snapshot copies use too.
        const auto& spec = m_Scene->GetFramebufferSpecification();
        const RHI::ResourceHandle source = context.ResolveTextureHandle(m_Source);
        const RHI::ResourceHandle destination = context.ResolveTextureHandle(m_Destination);
        if (!source.IsValid() || !destination.IsValid() || spec.Width == 0u || spec.Height == 0u)
            return;

        RenderCommand::CopyImageSubData(source, RendererAPI::TextureTargetType::Texture2D,
                                        destination, RendererAPI::TextureTargetType::Texture2D,
                                        spec.Width, spec.Height);
    }
} // namespace OloEngine
