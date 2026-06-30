#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/UpscalerRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <algorithm>
#include <span>

namespace OloEngine
{
    UpscalerRenderPass::UpscalerRenderPass()
    {
        SetName("UpscalerPass");
    }

    void UpscalerRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // Runs post-tonemap: prefer the freshest LDR colour. The candidate list
        // mirrors VignettePass (the stage that used to follow ToneMap directly),
        // so when CAS is disabled the chain naturally falls back to ToneMapColor.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ToneMapColor, ResourceNames::ToneMapColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ColorGradingColor, ResourceNames::ColorGradingColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ChromAbColor, ResourceNames::ChromAbColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FogColor, ResourceNames::FogColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
            });

        if (!m_Enabled)
            return;

        if (blackboard.Post.UpscalerColor.IsValid())
        {
            constexpr std::string_view upscalerVersionTag = "UpscalerPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.UpscalerColor, RGWriteUsage::RenderTarget, upscalerVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::UpscalerColorTexture) + "@" +
                                                            std::string(upscalerVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void UpscalerRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_CASShader = Shader::Create("assets/shaders/PostProcess_CAS.glsl");
        m_CASUBO = UniformBuffer::Create(CASUBOData::GetSize(), ShaderBindingLayout::UBO_UPSCALER);

        OLO_CORE_INFO("UpscalerRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void UpscalerRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        // The output framebuffer (UpscalerColor) is graph-owned: it is declared
        // in PopulateBlackboard and resolved per-frame in Execute via
        // GetPrimaryOutputFramebufferHandle. This pass never allocates one, so
        // m_Target stays null here and is only set in Execute. The dimension
        // guard just logs an obviously-bad spec (mirrors MotionBlur/TAA).
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("UpscalerRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void UpscalerRenderPass::Execute(RGCommandContext& context)
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
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer || !m_CASShader || !m_CASUBO)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

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

        m_CASShader->Bind();

        context.BindTexture(0, inputColorTextureID);
        m_CASShader->SetInt("u_Texture", 0);

        CASUBOData casData;
        casData.Params = glm::vec4(
            std::clamp(m_Settings.CASSharpness, 0.0f, 1.0f),
            outSpec.Width > 0u ? 1.0f / static_cast<f32>(outSpec.Width) : 0.0f,
            outSpec.Height > 0u ? 1.0f / static_cast<f32>(outSpec.Height) : 0.0f,
            0.0f);
        m_CASUBO->SetData(&casData, CASUBOData::GetSize());
        m_CASUBO->Bind();

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void UpscalerRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void UpscalerRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void UpscalerRenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
