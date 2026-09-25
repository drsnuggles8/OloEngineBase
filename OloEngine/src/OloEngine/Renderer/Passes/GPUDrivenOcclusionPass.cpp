#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ForwardScreenSpaceAOInputs.h"
#include "OloEngine/Renderer/Passes/GPUDrivenOcclusionPass.h"

#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"

#include <array>

namespace OloEngine
{
    GPUDrivenOcclusionPass::GPUDrivenOcclusionPass()
    {
        SetName("GPUDrivenOcclusionPass");
        OLO_CORE_INFO("Creating GPUDrivenOcclusionPass.");
    }

    void GPUDrivenOcclusionPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedSceneDepth = {};
        m_SelectedSceneNormals = {};

        // Forward / Forward+ only — the instanced batches are forward-lit PBR.
        // In Deferred they stay on the SceneRenderPass G-Buffer path.
        if (board.Config.Path == RenderingPath::Deferred)
            return;

        // Its forward PBR shaders apply screen-space AO to their ambient term (issue #1452).
        [[maybe_unused]] const bool readsForwardAO = ReadForwardScreenSpaceAOInputs(builder, board);

        // Re-export SceneDepth / SceneNormals after our draws so the AO / SSR
        // passes (which sample the exported textures) include this pass's
        // instanced geometry. Declared as TransferDest (we update them via
        // glCopyImageSubData in Execute) so the graph orders the AO reader after
        // us. ScenePass is the prior writer; we publish the newer version.
        if (board.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepth = board.Scene.SceneDepth;
            builder.Write(board.Scene.SceneDepth, RGWriteUsage::TransferDest);
        }
        if (board.Scene.SceneNormals.IsValid())
        {
            m_SelectedSceneNormals = board.Scene.SceneNormals;
            builder.Write(board.Scene.SceneNormals, RGWriteUsage::TransferDest);
        }

        // Declare the SceneColor RMW UNCONDITIONALLY in the forward path —
        // NOT gated on the per-frame bucket count. The HZB-occlusion toggle
        // flips at runtime without forcing a graph rebuild, so Setup may not
        // re-run when the bucket transitions empty<->non-empty; gating the
        // write on the bucket count would then leave this pass undeclared on
        // the flip frame. Declaring it every forward frame keeps the graph
        // topology stable (Execute no-ops on an empty bucket; the versioned
        // SceneColor handle is dependency-tracking only, so a no-op frame just
        // passes ScenePass's canonical SceneColor through unchanged).
        if (board.Scene.SceneColor.IsValid())
        {
            // Inter-pass RMW on the scene framebuffer: read the prior SceneColor
            // version, then advertise a renamed output via WriteNewVersion so the
            // validator does not see a same-pass feedback loop and downstream
            // name-based readers trace the dependency back to this pass. Mirrors
            // ForwardOverlayRenderPass. DependsOnPreviousWriter pins us right
            // after ScenePass (the prior SceneColor writer), so the occluders
            // are already in the depth buffer when our instanced survivors draw.
            SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
            [[maybe_unused]] const auto sceneColorRead = builder.Read(board.Scene.SceneColor, RGReadUsage::RenderTargetRead);
            constexpr std::string_view occlusionVersionTag = "GPUDrivenOcclusionPass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(board.Scene.SceneColor, RGWriteUsage::RenderTarget, occlusionVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }
    }

    void GPUDrivenOcclusionPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Per-pass command capture (issue #463): register + snapshot the bucket
        // BEFORE any early-return so even an empty frame appears in the breakdown.
        auto& captureManager = FrameCaptureManager::GetInstance();
        const bool capturing = captureManager.IsCapturing();
        if (capturing)
        {
            captureManager.BeginPass(GetName());
            captureManager.OnPreSort(m_CommandBucket);
        }

        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_SceneFramebuffer = resolvedSceneFB;
        }

        if (!m_SceneFramebuffer || m_CommandBucket.GetCommandCount() == 0)
        {
            // Drop any phase-2 work too — its packets are frame-allocator
            // CommandPacket*s that go stale next frame. (In practice phase-2 is
            // empty here, since phase-1 + phase-2 are submitted together, but
            // bail-out paths must not leave dangling references.)
            m_Phase2Packets.clear();
            m_Phase2Culls.clear();
            ResetCommandBucket();
            return;
        }

        GLStateGuard guard("GPUDrivenOcclusionPass", GLStateGuard::Policy::Ignore);

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        if (m_ForwardPrepassDrew)
        {
            // THE COLOUR HALF OF A SPLIT FRAME (issue #1452): the prepass node
            // drew both phases depth + view-normal and ran the phase-2 culls
            // against a complete depth buffer, so this replays the same draws
            // at GL_LEQUAL with depth writes off. Depth is unchanged, so only
            // the view normals are re-exported (the colour pass writes the
            // same values; the export keeps the post-colour version current).
            m_ForwardPrepassDrew = false;
            BindSceneForDraw(context);
            if (capturing)
                captureManager.OnPostSort(m_CommandBucket);
            CommandDispatch::SetDepthPrepassColorPassActive(true);
            m_CommandBucket.ExecuteParallel(rendererAPI);
            if (!m_Phase2Packets.empty())
                (void)CommandBucket::RecordPackets(rendererAPI, m_Phase2Packets);
            CommandDispatch::SetDepthPrepassColorPassActive(false);
            ExportDepthAndNormals(context, RGTextureHandle{}, m_SelectedSceneNormals);
        }
        else
        {
            BindSceneForDraw(context);
            m_CommandBucket.SortCommands();
            if (capturing)
                captureManager.OnPostSort(m_CommandBucket);
            DrawPhases(context, true);
            // Re-export depth + view-normals so AO / SSR include our instanced
            // geometry (#431). The framebuffer attachments now hold the
            // occluders + phase-1 + phase-2 survivors; copy them over ScenePass's
            // earlier export. Texture-to-texture copies — no framebuffer needed.
            ExportDepthAndNormals(context, m_SelectedSceneDepth, m_SelectedSceneNormals);
        }

        context.ResetOpaqueForwardDrawState();
        RenderCommand::SetBlendFuncSeparate(RHI::BlendFactor::One, RHI::BlendFactor::Zero,
                                            RHI::BlendFactor::One, RHI::BlendFactor::Zero);

        m_SceneFramebuffer->Unbind();
        RenderCommand::BindVertexArrayRaw(RHI::NullResource);
        RenderCommand::BindShaderProgram(RHI::NullResource);

        m_Phase2Packets.clear();
        m_Phase2Culls.clear();
        ResetCommandBucket();
    }

    void GPUDrivenOcclusionPass::BindSceneForDraw(RGCommandContext& context)
    {
        // The instanced batches are forward PBR meshes that write the full MRT
        // set: o_Color (0), o_EntityID (1), o_ViewNormal (2), o_Velocity (3).
        // Bind every color attachment the scene FB actually has so picking,
        // SSAO normals and TAA velocity are all repopulated.
        const auto& sceneSpec = m_SceneFramebuffer->GetSpecification();
        m_SceneColorAttachmentCount = 0;
        for (const auto& att : sceneSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++m_SceneColorAttachmentCount;
        }
        m_SceneFramebuffer->Bind();
        if (m_SceneColorAttachmentCount > 0)
        {
            RenderCommand::RestoreAllFramebufferDrawAttachments(m_SceneFramebuffer->GetRHIHandle(),
                                                                m_SceneColorAttachmentCount);
        }
        context.SetDepthTest(true);
        context.ResetOpaqueForwardDrawState();
        CommandDispatch::BindSceneResources();
    }

    void GPUDrivenOcclusionPass::DrawPhases(RGCommandContext& context, const bool cullPhase2)
    {
        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // Phase 1: the per-instance occlusion cull already ran at submission
        // (against the previous frame's retained Hi-Z), writing each batch's
        // surviving instances + indirect command. Executing the bucket replays
        // those indirect draws into the scene framebuffer, on top of the
        // non-instanced opaque geometry ScenePass already drew.
        m_CommandBucket.ExecuteParallel(rendererAPI);

        // Phase 2 (#431): the phase-1 draws are now in the depth buffer.
        // Rebuild a Hi-Z from the live framebuffer depth (occluders + phase-1
        // survivors), re-test each batch's reject list against it, and replay
        // the recovered (disoccluded) instances. This is the step that removes
        // the one-frame popping the single-phase scheme would show.
        if (m_Phase2Packets.empty() || !cullPhase2)
            return;

        const auto& sceneSpec = m_SceneFramebuffer->GetSpecification();
        const RHI::ResourceHandle depthTex = m_SceneFramebuffer->GetDepthAttachmentHandle();
        // Unbind so the depth attachment can be sampled by the Hi-Z compute
        // without an attachment/sampler feedback loop.
        m_SceneFramebuffer->Unbind();
        // The phase-1 draws just wrote this depth via the fixed-function
        // pipeline; the Hi-Z build samples it as a texture. Order the
        // framebuffer-write → texture-fetch (UE gets this from RDG; here it
        // is an explicit GL 4.5 texture barrier).
        RenderCommand::TextureBarrier();

        const GPUFrustumCuller::HZBOcclusionInputs currentHZB =
            Renderer3D::BuildCurrentOcclusionHZB(depthTex, sceneSpec.Width, sceneSpec.Height);

        // The Hi-Z build and the phase-2 culls rebind GL state, so the draw
        // state is re-established before the phase-2 packets (and kept sane
        // when the pyramid is unusable).
        const bool prepassActive = CommandDispatch::IsDepthPrepassActive();
        const bool writesNormals = CommandDispatch::DoesDepthPrepassWriteNormals();
        if (currentHZB.IsUsable())
        {
            for (const auto& cull : m_Phase2Culls)
                Renderer3D::DispatchOcclusionPhase2(cull, currentHZB);
            BindSceneForDraw(context);
            if (prepassActive)
                CommandDispatch::SetDepthPrepassActive(true, writesNormals);
            (void)CommandBucket::RecordPackets(rendererAPI, m_Phase2Packets);
        }
        else
        {
            BindSceneForDraw(context);
            if (prepassActive)
                CommandDispatch::SetDepthPrepassActive(true, writesNormals);
        }
    }

    void GPUDrivenOcclusionPass::ExportDepthAndNormals(RGCommandContext& context, const RGTextureHandle depthExport,
                                                       const RGTextureHandle normalsExport)
    {
        // Identities (issue #691) -- same unblock as SceneRenderPass's
        // exports: the destinations are graph transients.
        const auto& sceneSpec = m_SceneFramebuffer->GetSpecification();
        const RHI::ResourceHandle fbDepth = m_SceneFramebuffer->GetDepthAttachmentHandle();
        const RHI::ResourceHandle sceneDepthExport =
            depthExport.IsValid() ? context.ResolveTextureHandle(depthExport) : RHI::NullResource;
        if (sceneDepthExport.IsValid() && fbDepth.IsValid() && sceneDepthExport != fbDepth)
        {
            RenderCommand::CopyImageSubData(fbDepth, RendererAPI::TextureTargetType::Texture2D,
                                            sceneDepthExport, RendererAPI::TextureTargetType::Texture2D,
                                            sceneSpec.Width, sceneSpec.Height);
        }
        // RT2 is the octahedral view-normal attachment in the forward layout.
        const RHI::ResourceHandle fbNormals =
            m_SceneColorAttachmentCount > 2 ? m_SceneFramebuffer->GetColorAttachmentHandle(2) : RHI::NullResource;
        const RHI::ResourceHandle sceneNormalsExport =
            normalsExport.IsValid() ? context.ResolveTextureHandle(normalsExport) : RHI::NullResource;
        if (sceneNormalsExport.IsValid() && fbNormals.IsValid() && sceneNormalsExport != fbNormals)
        {
            RenderCommand::CopyImageSubData(fbNormals, RendererAPI::TextureTargetType::Texture2D,
                                            sceneNormalsExport, RendererAPI::TextureTargetType::Texture2D,
                                            sceneSpec.Width, sceneSpec.Height);
        }
    }

    void GPUDrivenOcclusionPass::SetupForwardPrepass(RGBuilder& builder, FrameBlackboard& board)
    {
        m_PrepassSceneDepth = {};
        m_PrepassSceneNormals = {};
        m_PrepassForwardAODepth = {};
        // Only with a forward AO buffer: that is the one consumer the prepass
        // share exists for. Without it this node declares nothing and the
        // pass draws in one go, exactly as before #1452.
        if (board.Config.Path == RenderingPath::Deferred || !board.AO.AOBuffer.IsValid() ||
            !board.Scene.ForwardAODepth.IsValid())
        {
            return;
        }
        builder.DependsOnPass("ScenePrepassPass");
        if (board.Scene.SceneColor.IsValid())
            builder.Write(board.Scene.SceneColor, RGWriteUsage::RenderTarget);
        if (board.Scene.SceneDepth.IsValid())
        {
            m_PrepassSceneDepth = board.Scene.SceneDepth;
            builder.Write(board.Scene.SceneDepth, RGWriteUsage::TransferDest);
        }
        if (board.Scene.SceneNormals.IsValid())
        {
            m_PrepassSceneNormals = board.Scene.SceneNormals;
            builder.Write(board.Scene.SceneNormals, RGWriteUsage::TransferDest);
        }
        m_PrepassForwardAODepth = board.Scene.ForwardAODepth;
        builder.Write(board.Scene.ForwardAODepth, RGWriteUsage::TransferDest);
    }

    void GPUDrivenOcclusionPass::ExecuteForwardPrepass(RGCommandContext& context, const Ref<Framebuffer>& sceneTarget)
    {
        OLO_PROFILE_FUNCTION();
        m_ForwardPrepassDrew = false;
        if (!m_PrepassForwardAODepth.IsValid())
            return;
        if (sceneTarget)
            m_SceneFramebuffer = sceneTarget;
        if (!m_SceneFramebuffer || m_CommandBucket.GetCommandCount() == 0)
            return;

        GLStateGuard guard("GPUDrivenOcclusionPrepassPass", GLStateGuard::Policy::Ignore);
        BindSceneForDraw(context);
        m_CommandBucket.SortCommands();
        CommandDispatch::SetDepthPrepassActive(true, true);
        DrawPhases(context, true);
        CommandDispatch::SetDepthPrepassActive(false);
        // The prepass masked every colour write; the AO passes that follow
        // must not inherit that (see SceneRenderPass::RunDepthPrepass).
        RenderCommand::GetRendererAPI().SetColorMask(true, true, true, true);
        CommandDispatch::InvalidateRenderStateCache();
        context.ResetOpaqueForwardDrawState();
        m_SceneFramebuffer->Unbind();

        // The prepass exports again, now with the instanced survivors in them:
        // the AO passes registered after this node read these versions.
        ExportDepthAndNormals(context, m_PrepassSceneDepth, m_PrepassSceneNormals);
        ExportDepthAndNormals(context, m_PrepassForwardAODepth, RGTextureHandle{});
        m_ForwardPrepassDrew = true;
    }

    void GPUDrivenOcclusionPass::SubmitPhase2(CommandPacket* packet, const GPUFrustumCuller::TwoPhaseCullResult& cull)
    {
        if (!packet)
            return;
        m_Phase2Packets.push_back(packet);
        m_Phase2Culls.push_back(cull);
    }

    Ref<Framebuffer> GPUDrivenOcclusionPass::GetTarget() const
    {
        return m_SceneFramebuffer;
    }

    void GPUDrivenOcclusionPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void GPUDrivenOcclusionPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void GPUDrivenOcclusionPass::OnReset()
    {
        // No own framebuffer to reset, but drop any queued phase-2 packets so a
        // graph reset / asset reload leaves no dangling frame-allocator pointers.
        m_Phase2Packets.clear();
        m_Phase2Culls.clear();
    }
} // namespace OloEngine
