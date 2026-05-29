#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    ToneMapRenderPass::ToneMapRenderPass()
    {
        SetName("ToneMapPass");
    }

    void ToneMapRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        m_SelectedSceneDepthTexture = {};
        // Scene depth feeds the underwater fog stage (eye-space distance for the
        // Beer-Lambert falloff). Depth is produced far upstream by the scene
        // pass, so reading it here doesn't reorder the post chain. The fog
        // stage self-skips in the shader when the camera is above water.
        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }

        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ColorGradingColor, ResourceNames::ColorGradingColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ChromAbColor, ResourceNames::ChromAbColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FogColor, ResourceNames::FogColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (blackboard.Post.ToneMapColor.IsValid())
        {
            constexpr std::string_view toneMapVersionTag = "ToneMapPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.ToneMapColor, RGWriteUsage::RenderTarget, toneMapVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::ToneMapColorTexture) + "@" +
                                                            std::string(toneMapVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void ToneMapRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");

        OLO_CORE_INFO("ToneMapRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ToneMapRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("ToneMapRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void ToneMapRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto fb = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = fb;
        }

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer || !m_Shader)
        {
            m_Target = nullptr;
            if (static u32 s_MissingInputWarnings = 0; outputFramebuffer && m_Shader && inputColorTextureID == 0u && s_MissingInputWarnings++ < 10)
            {
                OLO_CORE_WARN("ToneMapRenderPass: No valid setup-selected input texture resolved");
            }
            return;
        }

        m_Target = outputFramebuffer;

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

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

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        // Underwater fog UBO — bind so the shader stage can read the tint
        // params. The shader self-skips when the camera is above water, so a
        // missing/zeroed UBO is harmless. See §7.2.
        if (m_UnderwaterFogUBO)
            m_UnderwaterFogUBO->Bind();

        m_Shader->Bind();

        context.BindTexture(0, inputColorTextureID);
        m_Shader->SetInt("u_Texture", 0);

        // Scene depth for the underwater fog distance reconstruction.
        u32 depthTextureID = 0u;
        if (m_SelectedSceneDepthTexture.IsValid())
            depthTextureID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (depthTextureID != 0u)
        {
            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTextureID);
            m_Shader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);
        }

        // Per-pixel water-surface depth (nearest wavy surface) captured by the
        // water pass — lets the underwater fog find the real water boundary per
        // pixel instead of assuming a flat plane. 0 when no water rendered.
        const u32 waterDepthTextureID = Renderer3D::GetWaterSurfaceDepthTextureID();
        context.BindTexture(ShaderBindingLayout::TEX_UNDERWATER_WATER_DEPTH, waterDepthTextureID);
        m_Shader->SetInt("u_WaterSurfaceDepth", ShaderBindingLayout::TEX_UNDERWATER_WATER_DEPTH);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        // Leave the depth slot clean for subsequent passes that share the layout.
        if (depthTextureID != 0u)
            context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, 0);
        context.BindTexture(ShaderBindingLayout::TEX_UNDERWATER_WATER_DEPTH, 0);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void ToneMapRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ToneMapRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ToneMapRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
    }
} // namespace OloEngine
