#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ForwardScreenSpaceAOInputs.h"
#include "OloEngine/Renderer/Passes/FoliageRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
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

        if (!HasSubmittedCommands())
            return;

        // Its shaders apply screen-space AO to their ambient term (issue #1452).
        [[maybe_unused]] const bool readsForwardAO = ReadForwardScreenSpaceAOInputs(builder, board);

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

        const auto copyExport = [&context, this](RGTextureHandle handle, RHI::ResourceHandle source)
        {
            if (!handle.IsValid() || !source.IsValid())
                return;
            const auto destination = context.ResolveTextureHandle(handle);
            if (!destination.IsValid() || destination == source)
                return;
            const auto& spec = m_SceneFramebuffer->GetSpecification();
            RenderCommand::CopyImageSubData(source, RendererAPI::TextureTargetType::Texture2D,
                                            destination, RendererAPI::TextureTargetType::Texture2D,
                                            spec.Width, spec.Height);
        };
        copyExport(m_SelectedVelocityExport, m_SceneFramebuffer->GetColorAttachmentHandle(3));
        copyExport(m_SelectedSceneDepthExport, m_SceneFramebuffer->GetDepthAttachmentHandle());

        // Reset bucket for next frame
        ResetCommandBucket();
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
