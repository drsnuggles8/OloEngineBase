#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PreparedFullscreenPass.h"
#include "OloEngine/Renderer/Passes/DOFRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <span>

namespace OloEngine
{
    DOFRenderPass::DOFRenderPass()
    {
        SetName("DOFPass");
    }

    void DOFRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
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
        if (blackboard.Post.DOFColor.IsValid())
        {
            constexpr std::string_view dofVersionTag = "DOFPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.DOFColor, RGWriteUsage::RenderTarget, dofVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::DOFColorTexture) + "@" +
                                                            std::string(dofVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void DOFRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_DOFShader = Shader::Create("assets/shaders/PostProcess_DOF.glsl");

        OLO_CORE_INFO("DOFRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void DOFRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("DOFRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void DOFRenderPass::Execute(RGCommandContext& context)
    {
        auto prepared = PrepareParallelRecording(context);
        if (prepared.Record)
            prepared.Record(context);
    }

    RGPreparedPass DOFRenderPass::PrepareParallelRecording(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (!m_Enabled)
        {
            m_Target = nullptr;
            return {};
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !m_DOFShader)
        {
            m_Target = nullptr;
            return {};
        }

        const RHI::ResourceHandle sceneDepthTextureID = m_SelectedSceneDepthTexture.IsValid()
                                                            ? context.ResolveTextureHandle(m_SelectedSceneDepthTexture)
                                                            : RHI::NullResource;

        if (!sceneDepthTextureID.IsValid())
        {
            m_Target = nullptr;
            return {};
        }

        m_Target = outputFramebuffer;
        return PrepareFullscreenPass(outputFramebuffer, m_DOFShader,
                                     { { 0, inputColorTextureID, "u_Texture" }, { ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID, "u_DepthTexture" } },
                                     { m_PostProcessUBO });
    }

    void DOFRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void DOFRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void DOFRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
    }
} // namespace OloEngine
