#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

namespace OloEngine
{
    ColorGradingRenderPass::ColorGradingRenderPass()
    {
        SetName("ColorGradingPass");
    }

    void ColorGradingRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
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

        if (blackboard.ColorGradingColor.IsValid())
        {
            constexpr std::string_view colorGradingVersionTag = "ColorGradingPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.ColorGradingColor, RGWriteUsage::RenderTarget, colorGradingVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::ColorGradingColorTexture) + "@" +
                                                            std::string(colorGradingVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void ColorGradingRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_ColorGrading.glsl");

        OLO_CORE_INFO("ColorGradingRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ColorGradingRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("ColorGradingRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void ColorGradingRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        Ref<Framebuffer> inputFramebuffer;
        u32 inputColorTextureID = 0u;
        if (const auto inputHandle = GetPrimaryInputFramebufferHandle(); inputHandle.IsValid())
        {
            if (auto resolvedInput = context.ResolveFramebuffer(inputHandle))
                inputFramebuffer = resolvedInput;
        }
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
            m_Target = inputFramebuffer;
            return;
        }

        if (!inputFramebuffer || inputColorTextureID == 0u || !outputFramebuffer || !m_Shader)
        {
            m_Target = nullptr;
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

        m_Shader->Bind();

        context.BindTexture(0, inputColorTextureID);
        m_Shader->SetInt("u_Texture", 0);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void ColorGradingRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ColorGradingRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ColorGradingRenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
