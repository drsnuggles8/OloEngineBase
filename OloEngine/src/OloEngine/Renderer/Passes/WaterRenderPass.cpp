#include "OloEnginePCH.h"
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

    WaterRenderPass::~WaterRenderPass()
    {
        if (m_RefractionTextureID != 0)
        {
            glDeleteTextures(1, &m_RefractionTextureID);
            m_RefractionTextureID = 0;
        }
    }

    void WaterRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // No own framebuffer — this pass renders into the ScenePass target
    }

    void WaterRenderPass::EnsureRefractionTexture(u32 width, u32 height)
    {
        if (m_RefractionTextureID != 0 && m_RefractionWidth == width && m_RefractionHeight == height)
        {
            return;
        }

        if (m_RefractionTextureID != 0)
        {
            glDeleteTextures(1, &m_RefractionTextureID);
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &m_RefractionTextureID);
        glTextureStorage2D(m_RefractionTextureID, 1, GL_RGBA16F,
                           static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RefractionTextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        m_RefractionWidth = width;
        m_RefractionHeight = height;
    }

    void WaterRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

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

        // Detector guard — validates the pass restores FBO / blend / depth /
        // UBO / texture state. Guard is detector-only; the manual restores
        // in both OIT and forward branches below still perform the rollback.
        GLStateGuard guard("WaterRenderPass");

        u32 const fbWidth = m_SceneFramebuffer->GetSpecification().Width;
        u32 const fbHeight = m_SceneFramebuffer->GetSpecification().Height;

        // Guard against zero-sized framebuffers (minimized window, etc.)
        if (fbWidth == 0 || fbHeight == 0)
        {
            if (m_RefractionTextureID != 0)
            {
                glDeleteTextures(1, &m_RefractionTextureID);
                m_RefractionTextureID = 0;
                m_RefractionWidth = 0;
                m_RefractionHeight = 0;
            }
            ResetCommandBucket();
            return;
        }

        const bool useOIT = m_OITEnabled && m_OITBuffer && m_OITBuffer->GetFramebuffer() && m_OITShader;

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

            RenderCommand::SetDepthTest(true);
            RenderCommand::SetDepthFunc(GL_LEQUAL);
            RenderCommand::SetDepthMask(false);

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
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, sceneDepthID);
            u32 const sceneNormalsID = m_SceneFramebuffer->GetColorAttachmentRendererID(2);
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, sceneNormalsID);
            // Refraction sampler gets bound to the scene color FB directly
            // (no copy) — reading and writing the same texture is illegal,
            // but with OIT we accumulate separately so scene color is safe to sample.
            u32 const sceneColorID = m_SceneFramebuffer->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, sceneColorID);

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
            RenderCommand::SetBlendState(false);

            // Unbind the three texture slots we sampled into — leaving them
            // bound lets the refraction / scene-normals / water-depth slots
            // leak into subsequent passes that share the same sampler layout.
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, 0);
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, 0);
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, 0);

            RenderCommand::SetDepthMask(true);
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
        EnsureRefractionTexture(fbWidth, fbHeight);
        glCopyImageSubData(
            sceneColorID, GL_TEXTURE_2D, 0, 0, 0, 0,
            m_RefractionTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
            static_cast<GLsizei>(fbWidth), static_cast<GLsizei>(fbHeight), 1);

        m_SceneFramebuffer->Bind();

        // Bind scene depth for depth softening and shoreline foam
        u32 const depthTextureID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, depthTextureID);

        // Bind refraction color copy
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, m_RefractionTextureID);

        // Bind scene view-space normals for SSR ray marching
        u32 const normalsTextureID = m_SceneFramebuffer->GetColorAttachmentRendererID(2);
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, normalsTextureID);

        // Sort and dispatch water commands through the command bucket
        m_CommandBucket.SortCommands();

        auto& rendererAPI = RenderCommand::GetRendererAPI();
        m_CommandBucket.Execute(rendererAPI);

        // Restore render state after water (water uses blending + depth write off)
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthFunc(GL_LESS);
        RenderCommand::BackCull();
        CommandDispatch::InvalidateRenderStateCache();

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

    void WaterRenderPass::SetSceneFramebuffer(const Ref<Framebuffer>& fb)
    {
        OLO_PROFILE_FUNCTION();
        m_SceneFramebuffer = fb;
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
