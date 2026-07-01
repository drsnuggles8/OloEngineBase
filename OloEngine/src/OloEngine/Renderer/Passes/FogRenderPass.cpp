#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FogRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    FogRenderPass::FogRenderPass()
    {
        SetName("FogPass");
    }

    void FogRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedFogHalfResFramebuffer = {};
        m_SelectedFogHistoryTexture = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedShadowCSMTexture = {};

        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
            });

        if (!m_Enabled)
            return;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Post.UpscaledSceneDepthTexture.IsValid() ? blackboard.Post.UpscaledSceneDepthTexture : blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(m_SelectedSceneDepthTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.FogHistory.IsValid())
        {
            m_SelectedFogHistoryTexture = blackboard.Temporal.FogHistory;
            [[maybe_unused]] const auto fogHistoryRead = builder.Read(blackboard.Temporal.FogHistory, RGReadUsage::ShaderSample);
        }
        if (blackboard.Shadows.ShadowMapCSM.IsValid())
        {
            m_SelectedShadowCSMTexture = blackboard.Shadows.ShadowMapCSM;
            [[maybe_unused]] const auto shadowMapRead = builder.Read(blackboard.Shadows.ShadowMapCSM, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.FogHalfRes.IsValid())
        {
            m_SelectedFogHalfResFramebuffer = blackboard.Scratch.FogHalfRes;
            // Intra-pass write-then-sample: Pass A renders the half-resolution
            // ray-march into FogHalfRes; Pass B (bilateral upsample) samples
            // that result inside the same Execute. Graph-owned scratch with
            // no prior writer to chain against (history is consumed via
            // FogHistory, not FogHalfRes).
            builder.AllowSamePassReadWrite(blackboard.Scratch.FogHalfRes);
            builder.Write(blackboard.Scratch.FogHalfRes, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto fogHalfRead = builder.Read(blackboard.Scratch.FogHalfRes, RGReadUsage::ShaderSample);
            builder.ExtractHistoryTexture(ResourceNames::FogHistory, blackboard.Scratch.FogHalfRes);
        }

        if (blackboard.Post.FogColor.IsValid())
        {
            constexpr std::string_view fogVersionTag = "FogPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.FogColor, RGWriteUsage::RenderTarget, fogVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::FogColorTexture) + "@" +
                                                            std::string(fogVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void FogRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_FogShader = Shader::Create("assets/shaders/PostProcess_Fog.glsl");
        m_FogUpsampleShader = Shader::Create("assets/shaders/PostProcess_FogUpsample.glsl");

        OLO_CORE_INFO("FogRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void FogRenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("FogRenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            m_FogHalfWidth = 0;
            m_FogHalfHeight = 0;
            return;
        }

        // Half-resolution framebuffers for ray-march and temporal history.
        m_FogHalfWidth = (width + 1) / 2;
        m_FogHalfHeight = (height + 1) / 2;

        m_Target = nullptr;
    }

    void FogRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        Ref<Framebuffer> fogHalfResFramebuffer;
        u32 fogHistoryTextureID = 0;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedFogHalfResFramebuffer.IsValid())
            fogHalfResFramebuffer = context.ResolveFramebuffer(m_SelectedFogHalfResFramebuffer);
        if (m_SelectedFogHistoryTexture.IsValid())
            fogHistoryTextureID = context.ResolveTexture(m_SelectedFogHistoryTexture);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer || !m_FogShader || !m_FogUpsampleShader || !fogHalfResFramebuffer)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        const u32 sceneDepthTextureID = m_SelectedSceneDepthTexture.IsValid()
                                            ? context.ResolveTexture(m_SelectedSceneDepthTexture)
                                            : 0u;

        if (sceneDepthTextureID == 0)
            return; // Fog pass requires depth.

        // Placeholder sampler2DArrayShadow when no real CSM bound — shader's
        // u_DirectionalShadowEnabled still gates the actual sample.
        const u32 shadowCSMTextureID = m_SelectedShadowCSMTexture.IsValid()
                                           ? context.ResolveTexture(m_SelectedShadowCSMTexture)
                                           : ShadowMap::GetCSMPlaceholderRendererID();

        // Re-bind PostProcessUBO at binding 7 — IBL precompute and bloom-mip
        // updates can transiently claim this slot before the post-process chain.
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        // Re-bind the full shared camera UBO at binding 0. Both fog shaders read
        // the full CameraMatrices layout — u_CameraPosition (std140 offset 192,
        // PostProcess_Fog.glsl) and u_Projection (offset 128, PostProcess_Fog-
        // Upsample.glsl) — but an earlier 64-byte ViewProjection-only camera UBO
        // (Renderer2D / ParticleBatchRenderer style) can be left bound at slot 0,
        // which makes those reads out-of-bounds (origin-centred scenes survive
        // only because robust-access OOB reads return 0 ≈ the true camera). Pin
        // the full 272-byte UBO here so off-origin worlds fog correctly.
        if (m_CameraUBO)
            m_CameraUBO->Bind();

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
        context.BindTexture(0, inputColorTextureID);
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
        m_SelectedFogHalfResFramebuffer = {};
        m_SelectedFogHistoryTexture = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedShadowCSMTexture = {};
    }
} // namespace OloEngine
