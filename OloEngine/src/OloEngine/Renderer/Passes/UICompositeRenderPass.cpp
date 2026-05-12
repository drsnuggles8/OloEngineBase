#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include <span>

#include <glad/gl.h>

namespace OloEngine
{
    UICompositeRenderPass::UICompositeRenderPass()
    {
        SetName("UICompositePass");
        OLO_CORE_INFO("Creating UICompositeRenderPass.");
    }

    void UICompositeRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        if (blackboard.UIComposite.IsValid())
        {
            constexpr std::string_view uiCompositeVersionTag = "UICompositePass";
            const auto outputHandle =
                builder.WriteNewVersion(blackboard.UIComposite, RGWriteUsage::RenderTarget, uiCompositeVersionTag);
            if (outputHandle.IsValid())
            {
                SetPrimaryOutputFramebufferHandle(outputHandle);
                SetPrimaryOutputTextureHandle(
                    builder.CreateFramebufferAttachmentView(std::string(ResourceNames::UICompositeTexture) + "@" +
                                                                std::string(uiCompositeVersionTag),
                                                            outputHandle,
                                                            0u));
            }
        }

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SelectionOutlineColor, ResourceNames::SelectionOutlineColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FXAAColor, ResourceNames::FXAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::VignetteColor, ResourceNames::VignetteColorTexture),
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
    }

    void UICompositeRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");

        CreateFramebuffer(spec.Width, spec.Height);

        OLO_CORE_INFO("UICompositeRenderPass: Initialized {}x{}", spec.Width, spec.Height);
    }

    void UICompositeRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("UICompositeRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void UICompositeRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        Ref<Framebuffer> inputFramebuffer;
        u32 inputColorTextureID = 0u;
        const auto inputHandle = GetPrimaryInputFramebufferHandle();
        if (inputHandle.IsValid())
        {
            if (auto resolvedInput = context.ResolveFramebuffer(inputHandle))
                inputFramebuffer = resolvedInput;
        }
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);
        {
            static RGFramebufferHandle s_PreviousInputHandle{};
            static u32 s_PreviousInputFramebufferID = 0;
            static u32 s_PreviousInputColorID = 0;
            static u32 s_TransitionLogCount = 0;
            const u32 inputFramebufferID = inputFramebuffer ? inputFramebuffer->GetRendererID() : 0u;
            const u32 inputColorID = inputColorTextureID;
            if (inputHandle != s_PreviousInputHandle ||
                inputFramebufferID != s_PreviousInputFramebufferID ||
                inputColorID != s_PreviousInputColorID)
            {
                if (s_TransitionLogCount < 16)
                {
                    OLO_CORE_TRACE("UICompositePass: scene inputHandle=(idx={}, gen={}) fb={} colorTex={}",
                                   inputHandle.Index,
                                   inputHandle.Generation,
                                   inputFramebufferID,
                                   inputColorID);
                    ++s_TransitionLogCount;
                }
                s_PreviousInputHandle = inputHandle;
                s_PreviousInputFramebufferID = inputFramebufferID;
                s_PreviousInputColorID = inputColorID;
            }
        }

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto fb = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = fb;
        }

        if (!outputFramebuffer)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        // Bind our FBO and clear all attachments (handles mixed integer/float types)
        outputFramebuffer->Bind();
        const auto& outputSpec = outputFramebuffer->GetSpecification();
        context.SetViewport(0, 0, outputSpec.Width, outputSpec.Height);
        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        outputFramebuffer->ClearAllAttachments({ 0.0f, 0.0f, 0.0f, 1.0f }, -1);

        // Blit the post-processed scene as background
        if (inputColorTextureID != 0u && m_BlitShader)
        {
            m_BlitShader->Bind();
            context.BindTexture(0, inputColorTextureID);
            m_BlitShader->SetInt("u_Texture", 0);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }
        else if (m_NoInputWarningCount++ < 5)
        {
            OLO_CORE_WARN("UICompositePass: No input texture ({}) or blit shader ({}) — scene background will be black",
                          inputColorTextureID != 0u, m_BlitShader != nullptr);
        }

        // Render 2D overlays and screen-space UI via the per-frame callback
        if (m_RenderCallback)
        {
            context.SetBlendState(true);
            context.SetAlphaBlendStandard();
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetCulling(false);

            m_RenderCallback();

            // Restore GL state so later passes don't inherit altered state
            context.SetBlendState(false);
            context.SetOpaqueReplaceBlend();
            context.SetDepthTest(true);
            context.SetDepthMask(true);
            context.SetCulling(true);

            // One-shot: clear for next frame
            m_RenderCallback = nullptr;
        }
        else if (m_NoCallbackWarningCount++ < 5)
        {
            OLO_CORE_WARN("UICompositePass: No render callback set — UI will not render this frame");
        }
    }

    void UICompositeRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        CreateFramebuffer(width, height);

        OLO_CORE_INFO("UICompositeRenderPass: Setup {}x{}", width, height);
    }

    void UICompositeRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("UICompositeRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        CreateFramebuffer(width, height);

        OLO_CORE_INFO("UICompositeRenderPass: Resized to {}x{}", width, height);
    }

    void UICompositeRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;
    }

    void UICompositeRenderPass::SetRenderCallback(RenderCallback callback)
    {
        m_RenderCallback = std::move(callback);
    }
} // namespace OloEngine
