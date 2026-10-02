#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SceneAttachmentSnapshotPass.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    SceneAttachmentSnapshotPass::SceneAttachmentSnapshotPass(SceneRenderPass* scene, const Attachment attachment)
        : m_Scene(scene), m_Attachment(attachment)
    {
        SetName(attachment == Attachment::Depth ? "SceneDepthSnapshotPass" : "SceneViewNormalsSnapshotPass");
    }

    void SceneAttachmentSnapshotPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_Source = {};
        m_Snapshot = {};

        const RGTextureHandle source = m_Attachment == Attachment::Depth ? blackboard.Scene.SceneDepth
                                                                         : blackboard.Scene.SceneViewNormals;
        const RGTextureHandle snapshot = m_Attachment == Attachment::Depth ? blackboard.Scene.SceneDepthSnapshot
                                                                           : blackboard.Scene.SceneViewNormalsSnapshot;
        // The same handle means the source already is a texture the readers
        // may sample while drawing (Deferred's G-Buffer depth): nothing to copy.
        if (!source.IsValid() || !snapshot.IsValid() || source == snapshot)
            return;

        m_Source = source;
        m_Snapshot = snapshot;
        [[maybe_unused]] const auto sourceRead = builder.Read(source, RGReadUsage::TransferSource);
        builder.Write(snapshot, RGWriteUsage::TransferDest);
    }

    void SceneAttachmentSnapshotPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Scene || !m_Source.IsValid() || !m_Snapshot.IsValid())
            return;

        const auto& spec = m_Scene->GetFramebufferSpecification();
        const RHI::ResourceHandle source = context.ResolveTextureHandle(m_Source);
        const RHI::ResourceHandle snapshot = context.ResolveTextureHandle(m_Snapshot);
        if (!source.IsValid() || !snapshot.IsValid() || spec.Width == 0u || spec.Height == 0u)
            return;

        RenderCommand::CopyImageSubData(source, RendererAPI::TextureTargetType::Texture2D,
                                        snapshot, RendererAPI::TextureTargetType::Texture2D,
                                        spec.Width, spec.Height);
    }
} // namespace OloEngine
