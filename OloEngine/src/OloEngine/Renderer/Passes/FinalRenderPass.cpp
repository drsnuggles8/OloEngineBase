#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"

namespace OloEngine
{
    FinalRenderPass::FinalRenderPass()
    {
        SetName("FinalRenderPass");
        OLO_CORE_INFO("Creating FinalRenderPass.");
    }

    void FinalRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::UIComposite, ResourceNames::UICompositeTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SelectionOutlineColor, ResourceNames::SelectionOutlineColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FXAAColor, ResourceNames::FXAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::VignetteColor, ResourceNames::VignetteColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::UpscalerColor, ResourceNames::UpscalerColorTexture),
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
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (blackboard.Post.Backbuffer.IsValid())
            builder.Write(blackboard.Post.Backbuffer, RGWriteUsage::RenderTarget);
    }

    void FinalRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Load or create the blit shader
        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");

        // Phase E/F: Mark this pass as side-effecting (Present) so it's never culled.
        // The final pass writes to the swap-chain and must always execute.
        SetSideEffects(RenderGraphNode::SideEffect::Present);

        OLO_CORE_INFO("FinalRenderPass: Initialized with viewport dimensions {}x{}",
                      m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void FinalRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);
        context.ResetGraphicsStateToDefault();
        context.BindDefaultFramebuffer();
        context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        if (inputColorTextureID == 0u || !m_BlitShader)
        {
            if (static u32 s_MissingFinalInputWarnings = 0; s_MissingFinalInputWarnings++ < 10)
            {
                OLO_CORE_WARN("FinalRenderPass: missing input texture ({}) or blit shader ({})",
                              inputColorTextureID != 0u, m_BlitShader != nullptr);
            }
            return;
        }

        m_BlitShader->Bind();

        // Bind the setup-selected input texture view.
        if (inputColorTextureID == 0u)
        {
            static u32 s_InvalidFinalInputWarnings = 0;
            if (s_InvalidFinalInputWarnings++ < 10)
            {
                OLO_CORE_WARN("FinalRenderPass: setup-selected input texture view resolved to id=0");
            }
        }
        context.BindTexture(0, inputColorTextureID);
        m_BlitShader->SetInt("u_Texture", 0);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);
    }

    void FinalRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        OLO_CORE_INFO("FinalRenderPass setup with dimensions: {}x{}", width, height);
    }

    void FinalRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("FinalRenderPass::ResizeFramebuffer: Invalid dimensions: {}x{}", width, height);
            return;
        }

        // Update stored dimensions
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        OLO_CORE_INFO("FinalRenderPass: Resized viewport to {}x{}", width, height);
    }

    void FinalRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        // TODO: Recreate the fullscreen triangle and shader if needed
    }
} // namespace OloEngine
