#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    WaterRenderPass::WaterRenderPass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("WaterRenderPass");
        OLO_CORE_INFO("Creating WaterRenderPass.");
    }

    void WaterRenderPass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedSceneColorTexture = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedRefractionTexture = {};

        if (m_CommandBucket.GetCommandCount() == 0)
            return;

        if (board.Scene.SceneColor.IsValid())
        {
            // Inter-pass RMW: bind the prior SceneColor version as the
            // render target (resolved via GetPrimaryInputFramebufferHandle
            // in Execute) and advertise a renamed output. The SceneColorTexture
            // sample below provides the prior-version read. `WriteNewVersion`
            // republishes the base attachment views as versioned siblings; see
            // ForwardOverlayRenderPass for the rationale.
            SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
            constexpr std::string_view waterSceneColorVersionTag = "WaterPass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(board.Scene.SceneColor, RGWriteUsage::RenderTarget, waterSceneColorVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }

        if (board.Scene.SceneColorTexture.IsValid())
        {
            m_SelectedSceneColorTexture = board.Scene.SceneColorTexture;
            [[maybe_unused]] const auto sceneColorRead = builder.Read(board.Scene.SceneColorTexture, RGReadUsage::ShaderSample);
        }

        if (board.Scene.SceneDepthAttachment.IsValid())
        {
            m_SelectedSceneDepthTexture = board.Scene.SceneDepthAttachment;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(board.Scene.SceneDepthAttachment, RGReadUsage::ShaderSample);
        }

        if (board.Scene.SceneViewNormals.IsValid())
        {
            m_SelectedSceneNormalsTexture = board.Scene.SceneViewNormals;
            [[maybe_unused]] const auto sceneNormalsRead = builder.Read(board.Scene.SceneViewNormals, RGReadUsage::ShaderSample);
        }

        if (board.Scratch.WaterRefraction.IsValid())
        {
            m_SelectedRefractionTexture = board.Scratch.WaterRefraction;
            // Intra-pass transfer-then-sample: Execute glCopyImageSubData's
            // SceneColor → WaterRefraction and then samples WaterRefraction
            // as a shader resource within the same Execute. Graph-owned
            // scratch with no prior writer to chain against.
            builder.AllowSamePassReadWrite(board.Scratch.WaterRefraction);
            // glCopyImageSubData from SceneColor → WaterRefraction, then sampled
            // back as a shader resource — this is a transfer write, not an
            // image-store. ShaderImage would let the barrier planner schedule
            // an image-access fence instead of a copy-complete fence.
            builder.Write(board.Scratch.WaterRefraction, RGWriteUsage::TransferDest);
            [[maybe_unused]] const auto refractionRead = builder.Read(board.Scratch.WaterRefraction, RGReadUsage::ShaderSample);
        }
    }

    void WaterRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Clear any previously-published water-surface depth up front so the
        // underwater fog never samples a stale texture if this pass early-exits
        // (no scene FB, no water commands, zero-size, failed texture resolve)
        // before the capture runs. The successful capture path re-publishes it.
        Renderer3D::SetWaterSurfaceDepthTextureID(0);

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

        // Early out if no water commands were submitted this frame
        if (m_CommandBucket.GetCommandCount() == 0)
        {
            ResetCommandBucket();
            return;
        }

        // Temporary: keep WaterRenderPass diagnostics focused on functional
        // rendering errors while we migrate remaining command-bucket state
        // interactions. The pass still performs explicit state restores below.
        GLStateGuard guard("WaterRenderPass", GLStateGuard::Policy::Ignore);

        u32 const fbWidth = m_SceneFramebuffer->GetSpecification().Width;
        u32 const fbHeight = m_SceneFramebuffer->GetSpecification().Height;

        // Guard against zero-sized framebuffers (minimized window, etc.)
        if (fbWidth == 0 || fbHeight == 0)
        {
            ResetCommandBucket();
            return;
        }

        // Water always renders through the forward alpha-blend path now.
        // Copy scene colour for refraction sampling, then render into the
        // scene FB directly.
        // Phase D / H follow-up: resolve both the source scene attachments and
        // the water refraction scratch from graph-published handles only.
        u32 sceneColorID = 0u;
        u32 depthTextureID = 0u;
        u32 normalsTextureID = 0u;
        u32 refractionTexID = 0;
        if (m_SelectedSceneColorTexture.IsValid())
            sceneColorID = context.ResolveTexture(m_SelectedSceneColorTexture);
        if (m_SelectedSceneDepthTexture.IsValid())
            depthTextureID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (m_SelectedSceneNormalsTexture.IsValid())
            normalsTextureID = context.ResolveTexture(m_SelectedSceneNormalsTexture);
        if (m_SelectedRefractionTexture.IsValid())
            refractionTexID = context.ResolveTexture(m_SelectedRefractionTexture);
        if (sceneColorID == 0u || depthTextureID == 0u || normalsTextureID == 0u || refractionTexID == 0u)
        {
            ResetCommandBucket();
            return;
        }

        glCopyImageSubData(
            sceneColorID, GL_TEXTURE_2D, 0, 0, 0, 0,
            refractionTexID, GL_TEXTURE_2D, 0, 0, 0, 0,
            static_cast<GLsizei>(fbWidth), static_cast<GLsizei>(fbHeight), 1);

        m_SceneFramebuffer->Bind();

        // Bind scene depth for depth softening and shoreline foam
        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, depthTextureID);

        // Bind refraction color copy
        context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, refractionTexID);

        // Bind scene view-space normals for SSR ray marching
        context.BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, normalsTextureID);

        // Planar reflection (mirror) — bind the reflection colour target produced
        // by PlanarReflectionRenderPass and rebind its binding-43 UBO so the shader
        // has a live mirror VP + enable flag. The UBO's enable flag (set to 0 when
        // the pass is disabled / the texture id is 0) gates the shader, so a stale
        // texture is never sampled as a reflection.
        context.BindTexture(ShaderBindingLayout::TEX_WATER_PLANAR_REFLECTION,
                            Renderer3D::GetPlanarReflectionTextureID());
        if (m_PlanarReflectionUBO)
            m_PlanarReflectionUBO->Bind();

        // Sort and dispatch water commands through the command bucket
        m_CommandBucket.SortCommands();

        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // --- Water surface-depth capture ---------------------------------------
        // Re-render the same (already-sorted) water geometry depth-only into a
        // dedicated target so the underwater fog knows, per pixel, exactly where
        // the wavy surface is (the colour pass below writes no depth). The
        // CommandDispatch override forces depth-only state even though water is
        // blended; CommandBucket::Execute is re-entrant (it does not consume the
        // packets). The waterline discard still runs, so only the visible surface
        // side is captured. The bound scene textures stay bound (unit state).
        if (m_WaterDepthFB)
        {
            m_WaterDepthFB->Bind();
            glDepthMask(GL_TRUE);
            glClearDepth(1.0);
            glClear(GL_DEPTH_BUFFER_BIT); // far = "no water at this pixel"
            CommandDispatch::SetWaterDepthCaptureActive(true);
            m_CommandBucket.Execute(rendererAPI);
            CommandDispatch::SetWaterDepthCaptureActive(false);
            CommandDispatch::InvalidateRenderStateCache();
            m_WaterDepthFB->Unbind();
            Renderer3D::SetWaterSurfaceDepthTextureID(m_WaterDepthFB->GetDepthAttachmentRendererID());
            // Rebind the scene target for the colour pass below.
            m_SceneFramebuffer->Bind();
        }
        else
        {
            Renderer3D::SetWaterSurfaceDepthTextureID(0);
        }

        m_CommandBucket.Execute(rendererAPI);

        // Restore render state after water (water uses blending + depth write off)
        context.SetDepthMask(true);
        context.SetBlendState(false);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::BackCull();
        CommandDispatch::InvalidateRenderStateCache();

        // Unbind the three texture slots we sampled into — leaving them
        // bound lets water-depth / scene-normals / refraction slots leak
        // into subsequent passes that share the same sampler layout.
        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, 0);
        context.BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, 0);
        context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, 0);
        context.BindTexture(ShaderBindingLayout::TEX_WATER_PLANAR_REFLECTION, 0);

        m_SceneFramebuffer->Unbind();

        // Reset bucket for next frame
        ResetCommandBucket();
    }

    Ref<Framebuffer> WaterRenderPass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        // Return the ScenePass framebuffer since that's where we render
        return m_SceneFramebuffer;
    }

    void WaterRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        // Colour is rendered into the shared scene FBO (tracked dims for
        // consistency); we additionally own a depth-only target for the water
        // surface-depth capture used by the underwater fog.
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (width > 0 && height > 0)
        {
            FramebufferSpecification depthSpec;
            depthSpec.Width = width;
            depthSpec.Height = height;
            depthSpec.Attachments = { FramebufferTextureFormat::ShadowDepth };
            m_WaterDepthFB = Framebuffer::Create(depthSpec);
        }
    }

    void WaterRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (width > 0 && height > 0)
        {
            if (m_WaterDepthFB)
                m_WaterDepthFB->Resize(width, height);
            else
            {
                FramebufferSpecification depthSpec;
                depthSpec.Width = width;
                depthSpec.Height = height;
                depthSpec.Attachments = { FramebufferTextureFormat::ShadowDepth };
                m_WaterDepthFB = Framebuffer::Create(depthSpec);
            }
        }
    }

    void WaterRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        m_SelectedSceneColorTexture = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedRefractionTexture = {};
        // No own framebuffer to reset
    }
} // namespace OloEngine
