#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ForwardOverlayRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <glad/gl.h>

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
    }

    void ForwardOverlayRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        // Only runs when registered in the graph, which `Renderer3D::
        // ConfigureRenderGraph` does solely for RenderingPath::Deferred
        // (Forward/Forward+ route overlay draws through SceneRenderPass).
        // Remaining guards are for invalid-state safety only.
        if (!m_SceneFramebuffer || m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        m_SceneFramebuffer->Bind();

        const u32 sceneFBID = m_SceneFramebuffer->GetRendererID();
        const GLenum drawBuf = GL_COLOR_ATTACHMENT0;
        glNamedFramebufferDrawBuffers(sceneFBID, 1, &drawBuf);

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        rendererAPI.SetDepthTest(true);
        rendererAPI.SetDepthMask(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        rendererAPI.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // Rebind scene camera + light UBOs so overlay shaders see the same
        // view/projection/light data forward shaders expect.
        Renderer3D::BindSceneUBOs();

        // Sort and dispatch — skybox sorts last via its large depth key, so
        // terrain/voxel/grid render first, then the sky fills pixels with
        // depth == 1.0 via GL_LEQUAL (applied by the DrawSkybox dispatcher).
        if (m_CommandBucket.GetCommandCount() > 0)
        {
            m_CommandBucket.SortCommands();
            m_CommandBucket.Execute(rendererAPI);
        }

        // Restore multi-attachment draw buffers so later passes (e.g. decal
        // pass writing to RT1/RT2 of the scene FB, or TAA sampling RT3
        // velocity) see the full target set.
        const GLenum fullDrawBufs[] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2,
            GL_COLOR_ATTACHMENT3
        };
        glNamedFramebufferDrawBuffers(sceneFBID, 4, fullDrawBufs);

        rendererAPI.SetDepthMask(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        rendererAPI.SetBlendState(false);
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

    void ForwardOverlayRenderPass::SetSceneFramebuffer(const Ref<Framebuffer>& fb)
    {
        m_SceneFramebuffer = fb;
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
