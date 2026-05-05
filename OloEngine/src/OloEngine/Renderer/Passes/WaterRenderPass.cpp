#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Renderer.h"
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

    void WaterRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // No own framebuffer — this pass renders into the ScenePass target

        // Phase F slice 32 — read-modify-write into SceneColor so the hazard
        // validator can derive the DecalPass → WaterPass RAW ordering edge.
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
    }

    void WaterRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void WaterRenderPass::Execute(RGCommandContext& context)
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

        // Phase F slice 15 — fetch the OITBuffer through the provider
        // (lazy in OITResolveRenderPass). When OIT is disabled the
        // provider is not queried, leaving the cached Ref null.
        if (m_OITEnabled && m_OITBufferProvider)
            m_OITBuffer = m_OITBufferProvider();
        else
            m_OITBuffer.Reset();

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

        const bool oitRequested = m_OITEnabled && m_OITBuffer && m_OITBuffer->GetFramebuffer() && m_OITShader;
        const bool useOIT = false;
        if (oitRequested)
        {
            static u32 s_OITTemporarilyDisabledWarnings = 0;
            if (s_OITTemporarilyDisabledWarnings++ < 3)
            {
                OLO_CORE_WARN("WaterRenderPass: OIT path temporarily disabled; using forward water path for stability");
            }
        }

        if (useOIT)
        {
            // Weighted-blended OIT path. Water surfaces accumulate into the
            // shared OITBuffer (RGBA16F accum + RG16F revealage) with
            // per-attachment blend funcs; `OITResolveRenderPass` composites
            // the result over the scene FB. No refraction copy here —
            // `Water_OIT.glsl` pulls its colour blend from the accum buffer
            // instead of sampling a scene copy.
            Ref<Framebuffer> oitFB = m_OITBuffer->GetFramebuffer();

            m_OITBuffer->ClearForFrame(m_SceneFramebuffer);
            oitFB->Bind();

            context.SetDepthTest(true);
            RenderCommand::SetDepthFunc(GL_LEQUAL);
            context.SetDepthMask(false);

            RenderCommand::SetBlendStateForAttachment(0, true);
            RenderCommand::SetBlendStateForAttachment(1, true);
            RenderCommand::SetBlendFuncForAttachment(0, GL_ONE, GL_ONE);                  // accum: additive
            RenderCommand::SetBlendFuncForAttachment(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR); // revealage: multiplicative

            // Install Water_OIT program override directly on each queued
            // DrawWaterCommand packet. Keeping the override on the command
            // (instead of a global on CommandDispatch) preserves the
            // stateless, replay-safe contract of the bucket.
            const u32 oitProgramID = m_OITShader->GetRendererID();
            for (CommandPacket* packet : m_CommandBucket.GetPackets())
            {
                if (!packet || packet->GetCommandType() != CommandType::DrawWater)
                    continue;
                if (auto* cmd = packet->GetCommandData<DrawWaterCommand>())
                    cmd->oitProgramOverride = oitProgramID;
            }

            // Bind scene depth (for depth softening / shoreline foam) and
            // scene normals (for SSR). OIT variant still samples these.
            u32 const sceneDepthID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
            context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, sceneDepthID);
            u32 const sceneNormalsID = m_SceneFramebuffer->GetColorAttachmentRendererID(2);
            context.BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, sceneNormalsID);
            // Refraction sampler gets bound to the scene color FB directly
            // (no copy) — reading and writing the same texture is illegal,
            // but with OIT we accumulate separately so scene color is safe to sample.
            u32 const sceneColorID = m_SceneFramebuffer->GetColorAttachmentRendererID(0);
            context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, sceneColorID);

            m_CommandBucket.SortCommands();
            auto& rendererAPI = RenderCommand::GetRendererAPI();
            m_CommandBucket.Execute(rendererAPI);

            // Tell OITResolvePass there's fresh accumulation to composite.
            if (m_AccumMarker)
                m_AccumMarker();

            // Restore global blend state. GLStateGuard only *detects* leaks —
            // it doesn't roll state back — so we must explicitly reset both
            // the per-attachment blend enable and the per-attachment blend
            // function. Leaving attachment-1 at `(GL_ZERO, GL_ONE_MINUS_SRC_COLOR)`
            // would bleed through the next pass that re-enables blending on
            // that attachment (the OIT resolve composite, in particular).
            RenderCommand::SetBlendStateForAttachment(0, false);
            RenderCommand::SetBlendStateForAttachment(1, false);
            RenderCommand::SetBlendFuncForAttachment(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            RenderCommand::SetBlendFuncForAttachment(1, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            context.SetBlendState(false);

            // Unbind the three texture slots we sampled into — leaving them
            // bound lets the refraction / scene-normals / water-depth slots
            // leak into subsequent passes that share the same sampler layout.
            context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, 0);
            context.BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, 0);
            context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, 0);

            context.SetDepthMask(true);
            RenderCommand::SetDepthFunc(GL_LESS);
            RenderCommand::BackCull();
            CommandDispatch::InvalidateRenderStateCache();

            oitFB->Unbind();

            ResetCommandBucket();
            return;
        }

        // Classic forward alpha-blend path (default). Copy scene colour for
        // refraction sampling, then render water into the scene FB directly.
        // Copy scene color for refraction (before water renders over it)
        u32 const sceneColorID = m_SceneFramebuffer->GetColorAttachmentRendererID(0);

        // Phase D / H follow-up: resolve the water refraction scratch texture
        // from the transient pool only. The owned raw fallback texture has
        // been retired.
        u32 refractionTexID = 0;
        if (const auto* board = context.GetBlackboard())
        {
            if (board->WaterRefraction.IsValid())
                refractionTexID = context.ResolveTexture(board->WaterRefraction);
        }
        if (refractionTexID == 0)
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
        u32 const depthTextureID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, depthTextureID);

        // Bind refraction color copy
        context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, refractionTexID);

        // Bind scene view-space normals for SSR ray marching
        u32 const normalsTextureID = m_SceneFramebuffer->GetColorAttachmentRendererID(2);
        context.BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, normalsTextureID);

        // Sort and dispatch water commands through the command bucket
        m_CommandBucket.SortCommands();

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        m_CommandBucket.Execute(rendererAPI);

        // Restore render state after water (water uses blending + depth write off)
        context.SetDepthMask(true);
        context.SetBlendState(false);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::BackCull();
        CommandDispatch::InvalidateRenderStateCache();

        // Unbind the three texture slots we sampled into — leaving them
        // bound lets water-depth / scene-normals / refraction slots leak
        // into subsequent passes that share the same sampler layout. The
        // OIT branch above already performs the equivalent unbinds; mirror
        // them here so the detector `GLStateGuard("WaterRenderPass")` sees
        // matching entry/exit per-slot bindings regardless of which
        // transparency branch ran.
        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, 0);
        context.BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, 0);
        context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, 0);

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
        // No own framebuffer — dimensions tracked for consistency
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void WaterRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void WaterRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();
        // No own framebuffer to reset
    }
} // namespace OloEngine
