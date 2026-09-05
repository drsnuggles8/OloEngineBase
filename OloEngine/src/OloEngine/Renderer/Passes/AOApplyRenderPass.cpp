#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PreparedFullscreenPass.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"

#include <span>

namespace OloEngine
{
    AOApplyRenderPass::AOApplyRenderPass()
    {
        SetName("AOApplyPass");
        OLO_CORE_INFO("Creating AOApplyRenderPass.");
    }

    void AOApplyRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedAOTexture = {};
        m_SelectedSceneDepthTexture = {};

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (!m_Enabled || !blackboard.Post.AOApplyColor.IsValid() || !blackboard.AO.AOBuffer.IsValid() || !blackboard.Scene.SceneDepth.IsValid())
            return;

        [[maybe_unused]] const auto aoBufferRead = builder.Read(blackboard.AO.AOBuffer, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        m_SelectedAOTexture = blackboard.AO.AOBuffer;
        m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;

        constexpr std::string_view aoApplyVersionTag = "AOApplyPass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.AOApplyColor, RGWriteUsage::RenderTarget, aoApplyVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::AOApplyColorTexture) + "@" +
                                                        std::string(aoApplyVersionTag),
                                                    outputHandle,
                                                    0u));
    }

    void AOApplyRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        CreateFramebuffer(spec.Width, spec.Height);

        m_SSAOApplyShader = Shader::Create("assets/shaders/PostProcess_SSAOApply.glsl");

        OLO_CORE_INFO("AOApplyRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void AOApplyRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("AOApplyRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void AOApplyRenderPass::Execute(RGCommandContext& context)
    {
        auto prepared = PrepareParallelRecording(context);
        if (prepared.Record)
            prepared.Record(context);
    }

    RGPreparedPass AOApplyRenderPass::PrepareParallelRecording(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        RHI::ResourceHandle aoTextureID{};
        RHI::ResourceHandle sceneDepthID{};
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedAOTexture.IsValid())
            aoTextureID = context.ResolveTextureHandle(m_SelectedAOTexture);
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return {};
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer)
        {
            m_Target = nullptr;
            if (static u32 s_MissingInputOrOutputWarnings = 0; s_MissingInputOrOutputWarnings++ < 10)
            {
                OLO_CORE_WARN("AOApplyRenderPass: missing input/output (inputTex={}, outputFB={}, aoTex={}, depthTex={})",
                              inputColorTextureID,
                              outputFramebuffer ? outputFramebuffer->GetRHIHandle() : RHI::NullResource,
                              aoTextureID,
                              sceneDepthID);
            }
            OLO_CORE_ASSERT(false, "AOApplyRenderPass enabled without resolved graph input/output");
            return {};
        }

        if (const bool shaderReady = m_SSAOApplyShader && m_SSAOApplyShader->IsReady(); !shaderReady || !aoTextureID.IsValid() || !sceneDepthID.IsValid())
        {
            m_Target = nullptr;
            if (static u32 s_InvalidExecutionStateWarnings = 0; s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("AOApplyRenderPass: enabled without complete execution state (shaderReady={}, aoTex={}, depthTex={})",
                              shaderReady, aoTextureID, sceneDepthID);
            }
            OLO_CORE_ASSERT(false, "AOApplyRenderPass enabled without ready shader or resolved AO/depth inputs");
            return {};
        }

        m_Target = outputFramebuffer;
        return PrepareFullscreenPass(outputFramebuffer, m_SSAOApplyShader,
                                     { { 0, inputColorTextureID, {} }, { ShaderBindingLayout::TEX_SSAO, aoTextureID, {} }, { ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID, {} } },
                                     { m_PostProcessUBO, m_SSAOUBO });
    }

    void AOApplyRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void AOApplyRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void AOApplyRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedAOTexture = {};
        m_SelectedSceneDepthTexture = {};
    }

} // namespace OloEngine
