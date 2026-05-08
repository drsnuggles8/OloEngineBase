#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
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

    void ForwardOverlayRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec = spec;
        // No own framebuffer — we render into the scene FB supplied via
        // SetSceneFramebuffer(), populated with lit HDR by DeferredLightingPass.

        // Phase F slice 33 — read-modify-write into SceneColor so the hazard
        // validator can derive the DeferredLightingPass→ForwardOverlayPass and
        // ForwardOverlayPass→FoliagePass RAW ordering edges.
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
    }

    void ForwardOverlayRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void ForwardOverlayRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 36 — self-resolving SceneColor: look up directly
        // from the render graph blackboard so no per-frame side-channel
        // setter call is needed from EndScene().
        if (const auto* board = context.GetBlackboard())
        {
            if (board->SceneColor.IsValid())
            {
                if (auto resolvedSceneFB = context.ResolveFramebuffer(board->SceneColor))
                    m_SceneFramebuffer = resolvedSceneFB;
            }
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

        // Restoring guard: captures core GL state on entry (FBO / program /
        // depth / blend / stencil / cull / polygon / viewport / scissor)
        // and rolls it back in the destructor. Explicit restore calls
        // below remain in place for clarity and to keep invariants close
        // to the mutations, but the guard now also acts as a safety net if
        // a command path inside the bucket forgets a revert. Per-slot
        // texture/UBO bindings are NOT covered by ApplyCore — passes that
        // mutate those must still clean up themselves (which the overlay
        // pass does implicitly via its own SceneFramebuffer::Unbind).
        GLStateGuard guard("ForwardOverlayPass", GLStateGuard::Policy::Restore);

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
        const GLsizei overlayDrawBufCount =
            static_cast<GLsizei>(std::min<u32>(sceneColorAttachmentCount, static_cast<u32>(drawBufs.size())));
        if (overlayDrawBufCount > 0)
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

        m_SceneFramebuffer->Unbind();

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
