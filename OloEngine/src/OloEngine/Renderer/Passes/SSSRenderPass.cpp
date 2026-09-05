#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PreparedFullscreenPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <span>

namespace OloEngine
{
    SSSRenderPass::SSSRenderPass()
    {
        SetName("SSSPass");
    }

    void SSSRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};

        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidFramebufferTextureInputForPass(
            builder,
            this,
            RenderPipelineBuilderInternal::MakeFramebufferTextureInput(blackboard.Scene.SceneColor, blackboard.Scene.SceneColorTexture));

        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled || !blackboard.Post.SSSColor.IsValid())
            return;

        if (blackboard.Scene.SceneDepthAttachment.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepthAttachment;
            [[maybe_unused]] const auto depthRead = builder.Read(blackboard.Scene.SceneDepthAttachment, RGReadUsage::ShaderSample);
        }

        constexpr std::string_view sssVersionTag = "SSSPass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.SSSColor, RGWriteUsage::RenderTarget, sssVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::SSSColorTexture) + "@" +
                                                        std::string(sssVersionTag),
                                                    outputHandle,
                                                    0u));
    }

    void SSSRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Track framebuffer metadata for the graph-owned current-frame output.
        CreateOutputFramebuffer(spec.Width, spec.Height);

        // Load SSS blur shader
        m_SSSBlurShader = Shader::Create("assets/shaders/SSS_Blur.glsl");

        OLO_CORE_INFO("SSSRenderPass: Initialized with {}x{} framebuffer", spec.Width, spec.Height);
    }

    void SSSRenderPass::Execute(RGCommandContext& context)
    {
        auto prepared = PrepareParallelRecording(context);
        if (prepared.Record)
            prepared.Record(context);
    }

    RGPreparedPass SSSRenderPass::PrepareParallelRecording(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — only the input texture is sampled, and the pass
        // binds its own graph-owned output framebuffer.
        Ref<Framebuffer> outputFramebuffer;
        RHI::ResourceHandle inputColorTextureID{};
        RHI::ResourceHandle depthID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }

        if (m_SelectedSceneDepthTexture.IsValid())
            depthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);

        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
        {
            m_Target = nullptr;
            return {};
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !depthID.IsValid())
        {
            m_Target = nullptr;
            return {};
        }

        if (!IsReadyForExecution())
        {
            m_Target = nullptr;
            return {};
        }

        m_Target = outputFramebuffer;
        return PrepareFullscreenPass(outputFramebuffer, m_SSSBlurShader,
                                     { { 0, inputColorTextureID, {} }, { ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID, {} } },
                                     { m_SSSUBO }, false);
    }

    Ref<Framebuffer> SSSRenderPass::GetTarget() const
    {
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
        {
            return nullptr;
        }
        return m_Target;
    }

    void SSSRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateOutputFramebuffer(width, height);
    }

    void SSSRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        CreateOutputFramebuffer(width, height);
    }

    void SSSRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
    }

    void SSSRenderPass::CreateOutputFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }
} // namespace OloEngine
