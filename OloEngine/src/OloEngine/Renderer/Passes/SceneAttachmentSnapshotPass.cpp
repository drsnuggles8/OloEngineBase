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
        switch (attachment)
        {
            case Attachment::Depth:
                SetName("SceneDepthSnapshotPass");
                break;
            case Attachment::ViewNormals:
                SetName("SceneViewNormalsSnapshotPass");
                break;
            case Attachment::FluidDepth:
                SetName("FluidSceneDepthSnapshotPass");
                break;
            case Attachment::DiffusionDepth:
                SetName("DiffusionDepthSnapshotPass");
                break;
            case Attachment::DiffusionHandoff:
                SetName("DiffusionHandoffSnapshotPass");
                break;
        }
    }

    void SceneAttachmentSnapshotPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_Source = {};
        m_Snapshot = {};
        m_SceneFramebuffer = blackboard.Scene.SceneColor;

        RGTextureHandle source;
        RGTextureHandle snapshot;
        switch (m_Attachment)
        {
            case Attachment::Depth:
                source = blackboard.Scene.SceneDepth;
                snapshot = blackboard.Scene.SceneDepthSnapshot;
                break;
            case Attachment::ViewNormals:
                source = blackboard.Scene.SceneViewNormals;
                snapshot = blackboard.Scene.SceneViewNormalsSnapshot;
                break;
            case Attachment::FluidDepth:
                source = blackboard.Scene.SceneDepthAttachment;
                snapshot = blackboard.Scene.FluidSceneDepthSnapshot;
                break;
            case Attachment::DiffusionDepth:
                source = blackboard.Scene.SceneDepthAttachment;
                snapshot = blackboard.Scene.DiffusionDepthSnapshot;
                break;
            case Attachment::DiffusionHandoff:
                source = blackboard.Scene.SkinDiffuse;
                snapshot = blackboard.Scene.DiffusionHandoffSnapshot;
                break;
        }
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
        if (!m_Source.IsValid() || !m_Snapshot.IsValid())
            return;

        const auto framebuffer = context.ResolveFramebuffer(m_SceneFramebuffer);
        if (!framebuffer)
            return;
        const auto& spec = m_Scene ? m_Scene->GetFramebufferSpecification() : framebuffer->GetSpecification();
        const RHI::ResourceHandle source = context.ResolveTextureHandle(m_Source);
        const RHI::ResourceHandle snapshot = context.ResolveTextureHandle(m_Snapshot);
        if (!source.IsValid() || !snapshot.IsValid() || spec.Width == 0u || spec.Height == 0u)
            return;

        RenderCommand::CopyImageSubData(source, RendererAPI::TextureTargetType::Texture2D,
                                        snapshot, RendererAPI::TextureTargetType::Texture2D,
                                        spec.Width, spec.Height);
    }
} // namespace OloEngine
