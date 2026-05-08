#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <span>

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
            m_Target = nullptr;
            m_FogHistoryFB = nullptr;
            m_FogHalfWidth = 0;
            m_FogHalfHeight = 0;
            m_FogHistoryValid = false;
            return;
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
            m_FogHistoryFB = Framebuffer::Create(halfSpec);
        }

        m_Target = nullptr;
        m_FogHistoryValid = false;
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
        const auto* board = context.GetBlackboard();
        Ref<Framebuffer> inputFramebuffer;
        Ref<Framebuffer> outputFramebuffer;
        Ref<Framebuffer> fogHalfResFramebuffer;
        u32 fogHistoryTextureID = 0;
        if (board)
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

            if (board->FogColor.IsValid())
            {
                if (auto resolvedOutput = context.ResolveFramebuffer(board->FogColor))
                    outputFramebuffer = resolvedOutput;
            }

            if (board->FogHalfRes.IsValid())
            {
                if (auto resolvedHalfRes = context.ResolveFramebuffer(board->FogHalfRes))
                    fogHalfResFramebuffer = resolvedHalfRes;
            }

            fogHistoryTextureID = context.ResolveTexture(board->FogHistory);
        }

        if (!m_Enabled)
        {
            m_Target = inputFramebuffer;
            return;
        }

        if (!board || !board->FogColor.IsValid() || !board->FogHalfRes.IsValid() ||
            !inputFramebuffer || !outputFramebuffer || !m_FogShader || !m_FogUpsampleShader || !fogHalfResFramebuffer)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        // Phase F slice 42 / Phase H follow-up — self-resolve scene depth and
        // shadow map CSM from the render-graph blackboard.
        const u32 sceneDepthTextureID = context.ResolveTexture(board->SceneDepth);

        if (sceneDepthTextureID == 0)
            return; // Fog pass requires depth.

        const u32 shadowCSMTextureID = context.ResolveTexture(board->ShadowMapCSM);

        // Re-bind PostProcessUBO at binding 7 — IBL precompute and bloom-mip
        // updates can transiently claim this slot before the post-process chain.
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        // ----------------------------------------------------------------
        // Pass A — Half-resolution ray-march.
        // Output: RGBA16F (RGB = accumulated inscatter, A = transmittance).
        // ----------------------------------------------------------------
        fogHalfResFramebuffer->Bind();
        const auto& fogHalfSpec = fogHalfResFramebuffer->GetSpecification();
        context.SetViewport(0, 0, fogHalfSpec.Width, fogHalfSpec.Height);
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
        context.BindTexture(3, fogHistoryTextureID);

        // CSM shadow map for volumetric light shafts (slot TEX_SHADOW = 8).
        context.BindTexture(ShaderBindingLayout::TEX_SHADOW, shadowCSMTextureID);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }
        fogHalfResFramebuffer->Unbind();

        // ----------------------------------------------------------------
        // Pass B — Bilateral upsample + composite onto full-resolution scene.
        // ----------------------------------------------------------------
        outputFramebuffer->Bind();
        const auto& outSpec = outputFramebuffer->GetSpecification();
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

        const u32 fogID = fogHalfResFramebuffer->GetColorAttachmentRendererID(0);
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
        outputFramebuffer->Unbind();

        context.ExtractHistoryTexture(ResourceNames::FogHistory,
                                      board->FogHalfRes,
                                      [this](const u32 textureID)
                                      {
                                          StoreHistoryTexture(textureID);
                                      });
    }

    Ref<Framebuffer> FogRenderPass::GetTarget() const
    {
        if (!m_Target)
            return nullptr;
        return m_Target;
    }

    u32 FogRenderPass::GetFogHistoryTextureID() const
    {
        if (!m_FogHistoryValid || !m_FogHistoryFB)
            return 0;
        return m_FogHistoryFB->GetColorAttachmentRendererID(0);
    }

    void FogRenderPass::StoreHistoryTexture(const u32 textureID)
    {
        if (textureID == 0 || !m_FogHistoryFB)
            return;

        const u32 historyTextureID = m_FogHistoryFB->GetColorAttachmentRendererID(0);
        if (historyTextureID == 0)
            return;

        const auto& historySpec = m_FogHistoryFB->GetSpecification();
        if (historySpec.Width == 0 || historySpec.Height == 0)
            return;

        glCopyImageSubData(textureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                           historyTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                           static_cast<GLsizei>(historySpec.Width), static_cast<GLsizei>(historySpec.Height), 1);
        m_FogHistoryValid = true;
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
        m_Target = nullptr;
        m_FogHistoryValid = false;
    }
} // namespace OloEngine
