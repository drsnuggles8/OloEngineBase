#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FoliageRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Passes/ForwardScreenSpaceAOInputs.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"

namespace OloEngine
{
    FoliageRenderPass::FoliageRenderPass()
    {
        SetName("FoliageRenderPass");
        OLO_CORE_INFO("Creating FoliageRenderPass.");
    }

    void FoliageRenderPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedVelocityExport = {};
        m_SelectedSceneDepthExport = {};
        m_ReadsForwardAO = false;

        if (!HasSubmittedCommands())
            return;

        // The forward foliage programs apply screen-space AO to their ambient
        // term (issue #1474): FoliagePrepassPass put the leaves in the AO input.
        m_ReadsForwardAO = ReadForwardScreenSpaceAOInputs(builder, board);

        if (board.Scene.SceneColor.IsValid())
        {
            // Inter-pass RMW: read the prior SceneColor version, then
            // advertise a renamed output via WriteNewVersion so the
            // validator does not see a same-pass feedback loop.
            // `WriteNewVersion` republishes the base attachment views as
            // versioned siblings; see ForwardOverlayRenderPass for the
            // rationale.
            SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
            [[maybe_unused]] const auto sceneColorRead = builder.Read(board.Scene.SceneColor, RGReadUsage::RenderTargetRead);
            constexpr std::string_view foliageVersionTag = "FoliagePass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(board.Scene.SceneColor, RGWriteUsage::RenderTarget, foliageVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }
        // ScenePass exports before these forward draws. Republish the actual
        // attachments after foliage so temporal and depth consumers
        // see the same surface as SceneColor.
        const auto declareExport = [&builder](RGTextureHandle handle, RGTextureHandle& selected)
        {
            if (handle.IsValid())
            {
                selected = handle;
                builder.Write(handle, RGWriteUsage::TransferDest);
            }
        };
        // A deferred shader failure can route fallback overlays through this
        // pass. Its framebuffer does not contain the opaque G-Buffer velocity;
        // preserve those deferred exports rather than replacing the image.
        if (board.Config.Path != RenderingPath::Deferred)
        {
            declareExport(board.GBuffer.Velocity, m_SelectedVelocityExport);
            declareExport(board.Scene.SceneDepth, m_SelectedSceneDepthExport);
            // Page marking consumes the opaque depth snapshot. Complete that
            // reader before replacing it with the post-foliage depth image.
            if (m_SelectedSceneDepthExport.IsValid())
            {
                builder.DependsOnPass("VirtualShadowMapMarkPass");
                builder.DependsOnPass("SphereProxyAOPass");
                // The technique the graph was BUILT with (#771): the requested
                // one names an AO pass that may not be registered yet.
                switch (board.Config.GraphAOTechnique)
                {
                    case AOTechnique::SSAO:
                        builder.DependsOnPass("SSAOPass");
                        break;
                    case AOTechnique::GTAO:
                        builder.DependsOnPass("GTAOPass");
                        break;
                    case AOTechnique::None:
                        break;
                }
            }
        }
    }

    void FoliageRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Per-pass command capture (issue #463): register this pass and snapshot its
        // submission-order bucket BEFORE any early-return below, so even an empty
        // foliage frame appears in the frame breakdown's per-pass list.
        auto& captureManager = FrameCaptureManager::GetInstance();
        const bool capturing = captureManager.IsCapturing();
        if (capturing)
        {
            captureManager.BeginPass(GetName());
            captureManager.OnPreSort(m_CommandBucket);
        }

        // Resolve the setup-selected scene framebuffer instead of replaying
        // a blackboard lookup ladder at execute time.
        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_SceneFramebuffer = resolvedSceneFB;
        }

        if (!m_SceneFramebuffer)
        {
            ResetCommandBucket();
            return;
        }

        // Early out if no foliage commands were submitted this frame
        if (m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        m_SceneFramebuffer->Bind();

        // Republish the forward AO inputs (issue #1474) right before the colour
        // draws, as ScenePass does for its own: the passes between it and this
        // one may have rebound TEX_SSAO / TEX_POSTPROCESS_DEPTH. With no forward
        // AO this frame CommandDispatch binds white and the camera block says
        // it is not live.
        if (m_ReadsForwardAO)
            CommandDispatch::BindForwardScreenSpaceAO();

        // Sort and dispatch foliage commands through the command bucket
        m_CommandBucket.SortCommands();

        if (capturing)
            captureManager.OnPostSort(m_CommandBucket);

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        m_CommandBucket.ExecuteParallel(rendererAPI);

        // Restore defaults for subsequent passes
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        CommandDispatch::InvalidateRenderStateCache();

        m_SceneFramebuffer->Unbind();

        CopyToExport(context, m_SelectedVelocityExport, m_SceneFramebuffer->GetColorAttachmentHandle(3));
        CopyToExport(context, m_SelectedSceneDepthExport, m_SceneFramebuffer->GetDepthAttachmentHandle());

        // Reset bucket for next frame
        ResetCommandBucket();
    }

    void FoliageRenderPass::SetupForwardPrepass(RGBuilder& builder, FrameBlackboard& board)
    {
        // Only with a forward AO buffer, the one consumer this share exists
        // for — the same declaration GPUDrivenOcclusionPass's share makes.
        ForwardPrepassShareExports exports;
        DeclareForwardPrepassShare(builder, board, exports);
        m_PrepassSceneDepth = exports.SceneDepth;
        m_PrepassSceneNormals = exports.SceneNormals;
        m_PrepassForwardAODepth = exports.ForwardAODepth;
    }

    void FoliageRenderPass::ExecuteForwardPrepass(RGCommandContext& context, const Ref<Framebuffer>& sceneTarget)
    {
        OLO_PROFILE_FUNCTION();
        if (!m_PrepassForwardAODepth.IsValid())
            return;
        if (sceneTarget)
            m_SceneFramebuffer = sceneTarget;
        if (!m_SceneFramebuffer || m_CommandBucket.GetCommandCount() == 0)
            return;

        // The prepass is the bucket's FIRST touch this frame, so its capture
        // entry is the one that sees the submission order; Execute()'s then
        // records the sorted bucket its colour draws replay (issue #463).
        auto& captureManager = FrameCaptureManager::GetInstance();
        const bool capturing = captureManager.IsCapturing();
        if (capturing)
        {
            captureManager.BeginPass("FoliagePrepassPass");
            captureManager.OnPreSort(m_CommandBucket);
        }

        m_SceneFramebuffer->Bind();
        m_CommandBucket.SortCommands();
        if (capturing)
            captureManager.OnPostSort(m_CommandBucket);
        // Depth + view normal: DrawFoliageLayer swaps each forward foliage
        // program for its Foliage_*_DepthNormal twin and opens attachment 2.
        CommandDispatch::SetDepthPrepassActive(true, true);
        m_CommandBucket.ExecuteParallel(RenderCommand::GetRendererAPI());
        CommandDispatch::SetDepthPrepassActive(false);
        // The prepass masked every colour write; the AO passes that follow
        // must not inherit that (see SceneRenderPass::RunDepthPrepass).
        RenderCommand::GetRendererAPI().SetColorMask(true, true, true, true);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        CommandDispatch::InvalidateRenderStateCache();
        m_SceneFramebuffer->Unbind();

        // Export again, now with the leaves in them: the AO passes registered
        // after FoliagePrepassPass read these versions.
        const RHI::ResourceHandle depth = m_SceneFramebuffer->GetDepthAttachmentHandle();
        CopyToExport(context, m_PrepassSceneDepth, depth);
        CopyToExport(context, m_PrepassSceneNormals, m_SceneFramebuffer->GetColorAttachmentHandle(2));
        CopyToExport(context, m_PrepassForwardAODepth, depth);
    }

    void FoliageRenderPass::CopyToExport(RGCommandContext& context, RGTextureHandle handle, RHI::ResourceHandle source) const
    {
        if (!handle.IsValid() || !source.IsValid() || !m_SceneFramebuffer)
            return;
        const auto& spec = m_SceneFramebuffer->GetSpecification();
        if (spec.Width == 0u || spec.Height == 0u)
            return;
        const auto destination = context.ResolveTextureHandle(handle);
        if (!destination.IsValid() || destination == source)
            return;
        RenderCommand::CopyImageSubData(source, RendererAPI::TextureTargetType::Texture2D,
                                        destination, RendererAPI::TextureTargetType::Texture2D,
                                        spec.Width, spec.Height);
    }

    Ref<Framebuffer> FoliageRenderPass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        // Return the ScenePass framebuffer since that's where we render
        return m_SceneFramebuffer;
    }

    void FoliageRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer — dimensions tracked for consistency
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void FoliageRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void FoliageRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer to reset
    }
} // namespace OloEngine
