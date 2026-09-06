#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PreparedFullscreenPass.h"
#include "OloEngine/Renderer/Passes/ContactShadowRenderPass.h"
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
    ContactShadowRenderPass::ContactShadowRenderPass()
    {
        SetName("ContactShadowPass");
        OLO_CORE_INFO("Creating ContactShadowRenderPass.");
    }

    void ContactShadowRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};

        // Pick the latest upstream colour to darken: SSR (if reflections ran),
        // SSGI (if the indirect-diffuse bounce ran), AOApply (if AO ran), SSS,
        // else raw SceneColor. PostProcessColor is intentionally NOT a candidate —
        // its alias is repointed to ContactShadowColor downstream, so reading it
        // here would form a cycle.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSRColor, ResourceNames::SSRColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSGIColor, ResourceNames::SSGIColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        // ContactShadowColor is only declared (deferred path) when the G-Buffer
        // normal + depth are available; without them the pass cannot run and
        // downstream aliases back.
        if (!m_Enabled || !blackboard.Post.ContactShadowColor.IsValid() ||
            !blackboard.Scene.SceneDepth.IsValid() ||
            !blackboard.GBuffer.GBufferNormal.IsValid())
            return;

        [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto gbufferNormalRead = builder.Read(blackboard.GBuffer.GBufferNormal, RGReadUsage::ShaderSample);
        m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
        m_SelectedGBufferNormalTexture = blackboard.GBuffer.GBufferNormal;

        constexpr std::string_view contactShadowVersionTag = "ContactShadowPass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.ContactShadowColor, RGWriteUsage::RenderTarget, contactShadowVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::ContactShadowColorTexture) + "@" +
                                                        std::string(contactShadowVersionTag),
                                                    outputHandle,
                                                    0u));
    }

    void ContactShadowRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_ContactShadowShader = Shader::Create("assets/shaders/PostProcess_ContactShadow.glsl");

        OLO_CORE_INFO("ContactShadowRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ContactShadowRenderPass::Execute(RGCommandContext& context)
    {
        auto prepared = PrepareParallelRecording(context);
        if (prepared.Record)
            prepared.Record(context);
    }

    RGPreparedPass ContactShadowRenderPass::PrepareParallelRecording(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: the input framebuffer is intentionally not
        // resolved as an FBO here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        RHI::ResourceHandle sceneDepthID{};
        RHI::ResourceHandle gbufferNormalID{};
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedGBufferNormalTexture.IsValid())
            gbufferNormalID = context.ResolveTextureHandle(m_SelectedGBufferNormalTexture);

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
                OLO_CORE_WARN("ContactShadowRenderPass: missing input/output (inputTex={}, outputFB={}, depthTex={}, normalTex={})",
                              inputColorTextureID,
                              outputFramebuffer ? outputFramebuffer->GetRHIHandle() : RHI::NullResource,
                              sceneDepthID,
                              gbufferNormalID);
            }
            OLO_CORE_ASSERT(false, "ContactShadowRenderPass enabled without resolved graph input/output");
            return {};
        }

        if (const bool shaderReady = m_ContactShadowShader && m_ContactShadowShader->IsReady();
            !shaderReady || !sceneDepthID.IsValid() || !gbufferNormalID.IsValid())
        {
            m_Target = nullptr;
            if (static u32 s_InvalidExecutionStateWarnings = 0; s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("ContactShadowRenderPass: enabled without complete execution state (shaderReady={}, depthTex={}, normalTex={})",
                              shaderReady, sceneDepthID, gbufferNormalID);
            }
            OLO_CORE_ASSERT(false, "ContactShadowRenderPass enabled without ready shader or resolved G-Buffer/depth inputs");
            return {};
        }

        m_Target = outputFramebuffer;
        return PrepareFullscreenPass(outputFramebuffer, m_ContactShadowShader,
                                     { { 0, inputColorTextureID, {} }, { ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID, {} }, { ShaderBindingLayout::TEX_GBUFFER_NORMAL, gbufferNormalID, {} } },
                                     { m_ContactShadowUBO });
    }

    void ContactShadowRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ContactShadowRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void ContactShadowRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedGBufferNormalTexture = {};
    }

} // namespace OloEngine
