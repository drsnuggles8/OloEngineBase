#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"

#include <glad/gl.h>

#include <array>

namespace OloEngine
{
    ForwardOverlayRenderPass::ForwardOverlayRenderPass()
    {
        SetName("ForwardOverlayPass");
        OLO_CORE_INFO("Creating ForwardOverlayRenderPass.");
    }

    void ForwardOverlayRenderPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);

        if (Renderer3D::GetRendererSettings().Path != RenderingPath::Deferred)
            return;
        if (m_CommandBucket.GetCommandCount() == 0)
            return;

        if (board.Scene.SceneColor.IsValid())
        {
            // Inter-pass RMW: read the prior SceneColor version, then
            // advertise a renamed output via WriteNewVersion so the
            // validator does not see a same-pass feedback loop.
            // `WriteNewVersion` republishes the base attachment views as
            // versioned siblings, so downstream name-based readers
            // (`ReadFirstValidVersionedInputForPass`) trace the dependency
            // back to this pass even when every optional RMW chain step
            // (Foliage / Decal / Water / Particle / OITResolve) is empty.
            SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
            [[maybe_unused]] const auto sceneColorRead = builder.Read(board.Scene.SceneColor, RGReadUsage::RenderTargetRead);
            constexpr std::string_view forwardOverlayVersionTag = "ForwardOverlayPass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(board.Scene.SceneColor, RGWriteUsage::RenderTarget, forwardOverlayVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }
    }

    void ForwardOverlayRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Per-pass command capture (issue #463): register this pass and snapshot its
        // submission-order bucket BEFORE any early-return below, so even an empty
        // overlay frame appears in the frame breakdown's per-pass list.
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

        // Only runs when registered in the graph, which `Renderer3D::
        // ConfigureRenderGraph` does solely for RenderingPath::Deferred
        // (Forward/Forward+ route overlay draws through SceneRenderPass).
        // Remaining guards are for invalid-state safety only.
        if (!m_SceneFramebuffer || m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        // Silent guard: kept in scope so future leak hunts can flip the
        // policy back to Log/Restore without editing the pass. Policy::Ignore
        // is correct because the pass explicitly restores every critical
        // field at exit (depth/blend/blend-func/cull/polygon) and unbinds
        // its shader + VAO, so the only diffs Policy::Restore would surface
        // are the unbinds themselves (entry program/VAO != 0). The
        // SceneFramebuffer::Unbind() call below covers FBO state.
        GLStateGuard guard("ForwardOverlayPass", GLStateGuard::Policy::Ignore);

        m_SceneFramebuffer->Bind();

        const u32 sceneFBID = m_SceneFramebuffer->GetRendererID();

        // Count color attachments on the scene FB from its specification so
        // the full-layout restore below is exact regardless of the configured
        // attachment set (TAA velocity may or may not be present).
        const auto& sceneSpec = m_SceneFramebuffer->GetSpecification();
        u32 sceneColorAttachmentCount = 0;
        for (const auto& att : sceneSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++sceneColorAttachmentCount;
        }

        // Forward-overlay draws (skybox / terrain / voxel / non-PBR mesh /
        // grid / debug) use the forward fragment layout that writes o_Color
        // (location 0), o_EntityID (location 1) and o_ViewNormal (location 2).
        // Binding only attachment 0 silently discards the entity-ID and
        // view-normal writes — breaking picking for any entity rendered
        // through this pass and leaving stale SSAO/SSR normals in RT2.
        // Bind attachments 0-2 (clamped to what the scene FB actually has) so
        // those side buffers are repopulated per-frame. RT3 (velocity) is
        // intentionally left off: overlay shaders don't track motion vectors.
        std::array<GLenum, 3> drawBufs = {
            GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2
        };
        if (const GLsizei overlayDrawBufCount = static_cast<GLsizei>(std::min<u32>(sceneColorAttachmentCount, static_cast<u32>(drawBufs.size()))); overlayDrawBufCount > 0)
            glNamedFramebufferDrawBuffers(sceneFBID, overlayDrawBufCount, drawBufs.data());

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        context.SetDepthTest(true);
        context.SetDepthMask(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        context.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // Rebind shared scene resources so overlay shaders see the same
        // view/projection/light data forward shaders expect.
        CommandDispatch::BindSceneResources();

        // Sort and dispatch — skybox sorts last via its large depth key, so
        // terrain/voxel/grid render first, then the sky fills pixels with
        // depth == 1.0 via GL_LEQUAL (applied by the DrawSkybox dispatcher).
        if (m_CommandBucket.GetCommandCount() > 0)
        {
            m_CommandBucket.SortCommands();
            if (capturing)
                captureManager.OnPostSort(m_CommandBucket);
            m_CommandBucket.Execute(rendererAPI);
        }

        // Restore multi-attachment draw buffers built dynamically from the
        // scene FB spec so later passes (e.g. decal pass writing to RT1/RT2,
        // or TAA sampling RT3 velocity) see the full target set even if the
        // attachment count differs from the previous 4-entry hardcoded list.
        if (sceneColorAttachmentCount > 0)
        {
            std::array<GLenum, 16> fullDrawBufs{};
            const u32 n = std::min<u32>(sceneColorAttachmentCount, static_cast<u32>(fullDrawBufs.size()));
            for (u32 i = 0; i < n; ++i)
                fullDrawBufs[i] = GL_COLOR_ATTACHMENT0 + i;
            glNamedFramebufferDrawBuffers(sceneFBID, static_cast<GLsizei>(n), fullDrawBufs.data());
        }

        context.SetDepthMask(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        context.SetBlendState(false);
        // Restore cull face + polygon mode — skybox / debug commands inside the
        // bucket may flip these and would otherwise leak into the next pass.
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // Reset blend func to the default (GL_ONE, GL_ZERO). Bucket commands
        // (skybox / debug / grid) call glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
        // for alpha-blended draws and don't restore it; SetBlendState(false)
        // above disables blending but leaves the func sticky. Any downstream
        // pass that enables blending without setting its own func would inherit
        // the leak.
        ::glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);

        m_SceneFramebuffer->Unbind();

        // Unbind shader program + VAO so the GLStateGuard surfaces only
        // genuine regressions in downstream passes (the bucket's last
        // command leaves both bound).
        ::glBindVertexArray(0);
        ::glUseProgram(0);

        ResetCommandBucket();
    }

    Ref<Framebuffer> ForwardOverlayRenderPass::GetTarget() const
    {
        return m_SceneFramebuffer;
    }

    void ForwardOverlayRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ForwardOverlayRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ForwardOverlayRenderPass::OnReset()
    {
        // No own framebuffer to reset
    }
} // namespace OloEngine
