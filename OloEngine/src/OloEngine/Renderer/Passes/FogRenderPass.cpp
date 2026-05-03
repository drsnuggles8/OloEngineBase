#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    FogRenderPass::FogRenderPass()
    {
        SetName("FogPass");
    }

    void FogRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_FogShader = Shader::Create("assets/shaders/PostProcess_Fog.glsl");
        m_FogUpsampleShader = Shader::Create("assets/shaders/PostProcess_FogUpsample.glsl");

        DeclareRead(ResourceNames::PrecipitationColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::FogColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("FogRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void FogRenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("FogRenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_OutputFB = nullptr;
            m_FogHalfResFB = nullptr;
            m_FogHistoryFB = nullptr;
            m_FogHalfWidth = 0;
            m_FogHalfHeight = 0;
            return;
        }

        // Full-resolution output: composited scene colour.
        {
            FramebufferSpecification fbSpec;
            fbSpec.Width = width;
            fbSpec.Height = height;
            fbSpec.Samples = 1;
            fbSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
            m_OutputFB = Framebuffer::Create(fbSpec);
        }

        // Half-resolution framebuffers for ray-march and temporal history.
        m_FogHalfWidth = (width + 1) / 2;
        m_FogHalfHeight = (height + 1) / 2;

        {
            FramebufferSpecification halfSpec;
            halfSpec.Width = m_FogHalfWidth;
            halfSpec.Height = m_FogHalfHeight;
            halfSpec.Samples = 1;
            halfSpec.Attachments = { FramebufferTextureFormat::RGBA16F };
            m_FogHalfResFB = Framebuffer::Create(halfSpec);
            m_FogHistoryFB = Framebuffer::Create(halfSpec);
        }
    }

    void FogRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void FogRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 42 — self-resolving input framebuffer from the
        // render-graph blackboard. Preference chain matches prior EndScene()
        // setter: Precipitation > TAA > MotionBlur > DOF > Bloom > PostProcess.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->PrecipitationColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->TAAColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->MotionBlurColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->DOFColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->BloomColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->PostProcessColor))
                    inputFramebuffer = fb;
        }
        if (!m_Enabled || !inputFramebuffer || !m_OutputFB || !m_FogShader || !m_FogUpsampleShader || !m_FogHalfResFB)
        {
            return;
        }

        // Phase F slice 42 — self-resolve scene depth and shadow map CSM from
        // the render-graph blackboard.
        u32 sceneDepthTextureID = 0;
        if (const auto* board = context.GetBlackboard())
            sceneDepthTextureID = context.ResolveTexture(board->SceneDepth);
        if (sceneDepthTextureID == 0)
            sceneDepthTextureID = m_SceneDepthTextureID;

        if (sceneDepthTextureID == 0)
            return; // Fog pass requires depth.

        u32 shadowCSMTextureID = 0;
        if (const auto* board = context.GetBlackboard())
            shadowCSMTextureID = context.ResolveTexture(board->ShadowMapCSM);
        if (shadowCSMTextureID == 0)
            shadowCSMTextureID = m_ShadowCSMTextureID;

        // Re-bind PostProcessUBO at binding 7 — IBL precompute and bloom-mip
        // updates can transiently claim this slot before the post-process chain.
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        // ----------------------------------------------------------------
        // Pass A — Half-resolution ray-march.
        // Output: RGBA16F (RGB = accumulated inscatter, A = transmittance).
        // ----------------------------------------------------------------
        m_FogHalfResFB->Bind();
        context.SetViewport(0, 0, m_FogHalfWidth, m_FogHalfHeight);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        {
            constexpr u32 colorAttachment = 0;
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_FogShader->Bind();

        // Full-resolution depth (the shader samples at half-res UV).
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);

        // Temporal history for reprojection (slot 3).
        if (m_FogHistoryFB)
        {
            const u32 historyID = m_FogHistoryFB->GetColorAttachmentRendererID(0);
            context.BindTexture(3, historyID);
        }

        // CSM shadow map for volumetric light shafts (slot TEX_SHADOW = 8).
        if (shadowCSMTextureID != 0)
            context.BindTexture(ShaderBindingLayout::TEX_SHADOW, shadowCSMTextureID);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }
        m_FogHalfResFB->Unbind();

        // Swap ray-march result into history for next frame's temporal reprojection.
        // After the swap:
        //   m_FogHistoryFB  = this frame's result  (read next frame)
        //   m_FogHalfResFB  = previous history      (overwritten next frame)
        std::swap(m_FogHalfResFB, m_FogHistoryFB);

        // ----------------------------------------------------------------
        // Pass B — Bilateral upsample + composite onto full-resolution scene.
        // ----------------------------------------------------------------
        m_OutputFB->Bind();
        const auto& outSpec = m_OutputFB->GetSpecification();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        {
            constexpr u32 colorAttachment = 0;
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_FogUpsampleShader->Bind();

        // Scene colour at slot 0.
        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        m_FogUpsampleShader->SetInt("u_SceneColor", 0);

        // Half-res fog result is now in m_FogHistoryFB after the swap.
        const u32 fogID = m_FogHistoryFB->GetColorAttachmentRendererID(0);
        context.BindTexture(1, fogID);
        m_FogUpsampleShader->SetInt("u_FogTexture", 1);

        // Full-res depth for bilateral edge detection.
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);
        m_FogUpsampleShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }

        context.SetDepthMask(true);
        m_OutputFB->Unbind();
    }

    Ref<Framebuffer> FogRenderPass::GetTarget() const
    {
        if (!m_Enabled || !m_OutputFB)
            return nullptr;
        return m_OutputFB;
    }

    u32 FogRenderPass::GetFogHistoryTextureID() const
    {
        if (!m_FogHistoryFB)
            return 0;
        return m_FogHistoryFB->GetColorAttachmentRendererID(0);
    }

    void FogRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void FogRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void FogRenderPass::OnReset()
    {
    }
} // namespace OloEngine
