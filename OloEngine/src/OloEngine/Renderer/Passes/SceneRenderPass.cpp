#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Occlusion/OcclusionCuller.h"

namespace OloEngine
{
    SceneRenderPass::SceneRenderPass()
    {
        SetName("SceneRenderPass");
        OLO_CORE_INFO("Creating SceneRenderPass.");
    }

    void SceneRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Ensure the specification includes color and depth attachments
        if (m_FramebufferSpec.Attachments.Attachments.empty())
        {
            OLO_CORE_WARN("SceneRenderPass::Init: No attachments specified, adding default color and depth attachments");
            m_FramebufferSpec.Attachments = {
                FramebufferTextureFormat::RGBA8, // Color buffer
                FramebufferTextureFormat::Depth  // Depth attachment
            };
        }

        m_Target = Framebuffer::Create(m_FramebufferSpec);

        OLO_CORE_INFO("SceneRenderPass: Created framebuffer with dimensions {}x{}",
                      m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void SceneRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Target)
        {
            OLO_CORE_ERROR("SceneRenderPass::Execute: No target framebuffer!");
            return;
        }

        m_Target->Bind();

        // Clear all attachments properly (handles mixed integer/float attachments)
        // This clears color attachments with the specified color, entity ID with -1, and depth/stencil
        m_Target->ClearAllAttachments({ 0.1f, 0.1f, 0.1f, 1.0f }, -1);

        // Reset to default OpenGL state to ensure consistent rendering
        auto& rendererAPI = RenderCommand::GetRendererAPI();
        rendererAPI.SetDepthTest(true);
        rendererAPI.SetDepthFunc(GL_LESS);
        rendererAPI.SetDepthMask(true);
        rendererAPI.SetBlendState(false);
        rendererAPI.SetCullFace(GL_BACK);
        rendererAPI.SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        // Capture hooks — minimal overhead when not capturing (helped by branch prediction)
        auto& captureManager = FrameCaptureManager::GetInstance();
        const bool capturing = captureManager.IsCapturing();

        if (capturing)
            captureManager.OnPreSort(m_CommandBucket);

        // BatchCommands uses hash-table grouping (O(n)) which doesn't require
        // pre-sorted input, and sorts internally afterward. Skipping the separate
        // SortCommands() call avoids a redundant sort pass.
        // When batching is disabled, fall back to sort-only.
        if (m_CommandBucket.GetCommandCount() > 0)
        {
            m_CommandBucket.BatchCommands(*m_Allocator);

            // If batching was disabled or no-op, ensure we're still sorted
            if (!m_CommandBucket.IsSorted())
                m_CommandBucket.SortCommands();
        }

        if (capturing)
            captureManager.OnPostSort(m_CommandBucket);

        // Invoke post-batch capture step when capturing is active
        if (capturing)
            captureManager.OnPostBatch(m_CommandBucket);

        // Re-bind scene camera & light UBOs that earlier passes (e.g. ShadowPass)
        // may have overwritten at the same binding points.
        Renderer3D::BindSceneUBOs();

        // Depth prepass: render all geometry depth-only first, then re-execute
        // with GL_EQUAL and no depth writes for the color pass. This eliminates
        // overdraw from fragment shading of occluded pixels.
        const bool depthPrepass = Renderer3D::IsDepthPrepassEnabled();
        if (depthPrepass)
        {
            // Pass 1: depth only — CommandDispatch overrides per-command state
            CommandDispatch::SetDepthPrepassActive(true);
            m_CommandBucket.Execute(rendererAPI);
            CommandDispatch::SetDepthPrepassActive(false);
        }

        // Flush deferred occlusion query proxy draws. When a depth prepass ran,
        // the depth buffer is fully populated; otherwise the first Execute below
        // will populate it and queries will rely on the previous frame's depth.
        if (Renderer3D::IsOcclusionCullingEnabled())
        {
            OcclusionCuller::GetInstance().FlushQueuedQueries();
        }

        // Forward+ light culling: dispatch compute after depth is available
        auto& forwardPlus = Renderer3D::GetForwardPlus();
        if (forwardPlus.ShouldUseForwardPlus() && depthPrepass)
        {
            const u32 depthTexID = m_Target->GetDepthAttachmentRendererID();
            forwardPlus.DispatchCulling(
                Renderer3D::GetViewMatrix(),
                Renderer3D::GetProjectionMatrix(),
                depthTexID);
            forwardPlus.BindForShading();
        }

        // Set up color pass state AFTER occlusion flush (which mutates GL state)
        if (depthPrepass)
        {
            // Pass 2: color — CommandDispatch overrides per-command depth state
            // to GL_LEQUAL + depth mask false so fragments at the same depth as
            // the prepass pass the test while preventing new depth writes.
            CommandDispatch::SetDepthPrepassColorPassActive(true);
        }

        if (capturing)
            m_CommandBucket.ExecuteWithGPUTiming(rendererAPI);
        else
            m_CommandBucket.Execute(rendererAPI);

        // Restore depth state after prepass
        if (depthPrepass)
        {
            CommandDispatch::SetDepthPrepassColorPassActive(false);
            rendererAPI.SetDepthMask(true);
            rendererAPI.SetDepthFunc(GL_LESS);
        }

        // Unbind Forward+ SSBOs after the color pass
        if (forwardPlus.ShouldUseForwardPlus() && depthPrepass)
        {
            // Render debug heatmap overlay before unbinding (needs grid SSBO + UBO)
            if (forwardPlus.IsDebugVisualization())
            {
                auto quadVAO = Renderer3D::GetFullscreenQuadVAO();
                auto debugShader = Renderer3D::GetForwardPlusDebugShader();
                if (quadVAO && debugShader)
                {
                    forwardPlus.RenderDebugOverlay(quadVAO->GetRendererID(), debugShader);
                }
            }
            forwardPlus.UnbindAfterShading();
        }

        static u32 s_FrameCounter = 0;
        s_FrameCounter++;

        if (capturing)
        {
            captureManager.OnFrameEnd(
                s_FrameCounter,
                m_CommandBucket.GetLastSortTimeMs(),
                m_CommandBucket.GetLastBatchTimeMs(),
                m_CommandBucket.GetLastExecuteTimeMs());
        }

        m_Target->Unbind();
    }

    Ref<Framebuffer> SceneRenderPass::GetTarget() const
    {
        return m_Target;
    }

    void SceneRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::SetupFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        // Create or recreate the framebuffer
        if (!m_Target)
        {
            Init(m_FramebufferSpec);
        }
        else
        {
            m_Target->Resize(width, height);
        }
    }

    void SceneRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SceneRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        if (m_Target)
        {
            m_Target->Resize(width, height);
            OLO_CORE_INFO("SceneRenderPass: Resized framebuffer to {}x{}", width, height);
        }
    }

    void SceneRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        // Recreate the framebuffer with current specs
        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            OLO_CORE_INFO("SceneRenderPass reset with framebuffer dimensions: {}x{}",
                          m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            Init(m_FramebufferSpec);
        }
    }
} // namespace OloEngine
