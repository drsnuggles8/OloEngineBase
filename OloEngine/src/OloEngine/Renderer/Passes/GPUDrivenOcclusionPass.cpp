#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GPUDrivenOcclusionPass.h"

#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"

#include <glad/gl.h>

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
        if (Renderer3D::GetRendererSettings().Path == RenderingPath::Deferred)
            return;

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
        const u32 sceneFBID = m_SceneFramebuffer->GetRendererID();

        // The instanced batches are forward PBR meshes that write the full MRT
        // set: o_Color (0), o_EntityID (1), o_ViewNormal (2), o_Velocity (3).
        // Bind every color attachment the scene FB actually has so picking,
        // SSAO normals and TAA velocity are all repopulated.
        const auto& sceneSpec = m_SceneFramebuffer->GetSpecification();
        u32 sceneColorAttachmentCount = 0;
        for (const auto& att : sceneSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++sceneColorAttachmentCount;
        }

        // Bind the scene FB + the forward MRT draw-buffer set + opaque depth
        // state + shared scene resources. Replayed before phase 1 and again
        // before phase 2 (the mid-frame Hi-Z + phase-2 culls rebind GL state).
        const auto bindSceneForDraw = [&]()
        {
            m_SceneFramebuffer->Bind();
            if (sceneColorAttachmentCount > 0)
            {
                std::array<GLenum, 16> drawBufs{};
                const u32 n = std::min<u32>(sceneColorAttachmentCount, static_cast<u32>(drawBufs.size()));
                for (u32 i = 0; i < n; ++i)
                    drawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
                glNamedFramebufferDrawBuffers(sceneFBID, static_cast<GLsizei>(n), drawBufs.data());
            }
            context.SetDepthTest(true);
            context.SetDepthMask(true);
            rendererAPI.SetDepthFunc(GL_LESS);
            context.SetBlendState(false);
            rendererAPI.SetCullFace(GL_BACK);
            rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            CommandDispatch::BindSceneResources();
        };

        bindSceneForDraw();

        // Phase 1: the per-instance occlusion cull already ran at submission
        // (against the previous frame's retained Hi-Z), writing each batch's
        // surviving instances + indirect command. Executing the bucket replays
        // those indirect draws into the scene framebuffer, on top of the
        // non-instanced opaque geometry ScenePass already drew.
        m_CommandBucket.SortCommands();
        if (capturing)
            captureManager.OnPostSort(m_CommandBucket);
        m_CommandBucket.Execute(rendererAPI);

        // Phase 2 (#431 Stage 2): the phase-1 draws are now in the depth buffer.
        // Rebuild a Hi-Z from the live framebuffer depth (occluders + phase-1
        // survivors), re-test each batch's reject list against it, and replay
        // the recovered (disoccluded) instances. This is the step that removes
        // the one-frame popping the single-phase scheme would show.
        if (!m_Phase2Packets.empty())
        {
            const u32 depthTexID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
            // Unbind so the depth attachment can be sampled by the Hi-Z compute
            // without an attachment/sampler feedback loop.
            m_SceneFramebuffer->Unbind();
            // The phase-1 draws just wrote this depth via the fixed-function
            // pipeline; the Hi-Z build samples it as a texture. Order the
            // framebuffer-write → texture-fetch (UE gets this from RDG; here it
            // is an explicit GL 4.5 texture barrier).
            ::glTextureBarrier();

            const GPUFrustumCuller::HZBOcclusionInputs currentHZB =
                Renderer3D::BuildCurrentOcclusionHZB(depthTexID, sceneSpec.Width, sceneSpec.Height);

            if (currentHZB.IsUsable())
            {
                for (const auto& cull : m_Phase2Culls)
                    Renderer3D::DispatchOcclusionPhase2(cull, currentHZB);

                bindSceneForDraw();
                for (CommandPacket* packet : m_Phase2Packets)
                    if (packet)
                        packet->Execute(rendererAPI);
            }
            else
            {
                bindSceneForDraw(); // keep the unbind below balanced / state sane
            }
        }

        // Re-export depth + view-normals so AO / SSR include our instanced
        // geometry (#431 Stage 3). The framebuffer attachments now hold the
        // occluders + phase-1 + phase-2 survivors; copy them over ScenePass's
        // earlier export. Texture-to-texture copies — no framebuffer needed.
        {
            const u32 fbDepthID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
            const u32 sceneDepthExportID = m_SelectedSceneDepth.IsValid() ? context.ResolveTexture(m_SelectedSceneDepth) : 0u;
            if (sceneDepthExportID != 0u && fbDepthID != 0u && sceneDepthExportID != fbDepthID)
            {
                ::glCopyImageSubData(fbDepthID, GL_TEXTURE_2D, 0, 0, 0, 0,
                                     sceneDepthExportID, GL_TEXTURE_2D, 0, 0, 0, 0,
                                     static_cast<GLsizei>(sceneSpec.Width), static_cast<GLsizei>(sceneSpec.Height), 1);
            }
            // RT2 is the octahedral view-normal attachment in the forward layout.
            const u32 fbNormalsID = sceneColorAttachmentCount > 2 ? m_SceneFramebuffer->GetColorAttachmentRendererID(2) : 0u;
            const u32 sceneNormalsExportID = m_SelectedSceneNormals.IsValid() ? context.ResolveTexture(m_SelectedSceneNormals) : 0u;
            if (sceneNormalsExportID != 0u && fbNormalsID != 0u && sceneNormalsExportID != fbNormalsID)
            {
                ::glCopyImageSubData(fbNormalsID, GL_TEXTURE_2D, 0, 0, 0, 0,
                                     sceneNormalsExportID, GL_TEXTURE_2D, 0, 0, 0, 0,
                                     static_cast<GLsizei>(sceneSpec.Width), static_cast<GLsizei>(sceneSpec.Height), 1);
            }
        }

        context.SetDepthMask(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        context.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        ::glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);

        m_SceneFramebuffer->Unbind();
        ::glBindVertexArray(0);
        ::glUseProgram(0);

        m_Phase2Packets.clear();
        m_Phase2Culls.clear();
        ResetCommandBucket();
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
