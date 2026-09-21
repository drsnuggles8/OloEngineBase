#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/TAARenderPass.h"

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
    TAARenderPass::TAARenderPass()
    {
        SetName("TAAPass");
    }

    void TAARenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSurfaceHistoryTexture = {};

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
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
        if (blackboard.GBuffer.Velocity.IsValid())
        {
            m_SelectedVelocityTexture = blackboard.Post.UpscaledVelocityTexture.IsValid() ? blackboard.Post.UpscaledVelocityTexture : blackboard.GBuffer.Velocity;
            [[maybe_unused]] const auto velocityRead = builder.Read(m_SelectedVelocityTexture, RGReadUsage::ShaderSample);

            // Keep the exact RT3 this resolve sampled, so next frame compares
            // against the coverage it actually blended rather than a
            // differently-upscaled one. Declared from Setup for the reason
            // RayTracedShadowPass documents: BuildFrameGraph clears extraction
            // contracts before visiting nodes, so an earlier declaration is
            // discarded on every cache miss.
            builder.ExtractHistoryTexture(ResourceNames::TAASurfaceHistory, m_SelectedVelocityTexture);
        }
        if (blackboard.Temporal.TAASurfaceHistory.IsValid())
        {
            m_SelectedSurfaceHistoryTexture = blackboard.Temporal.TAASurfaceHistory;
            [[maybe_unused]] const auto surfaceRead =
                builder.Read(m_SelectedSurfaceHistoryTexture, RGReadUsage::ShaderSample);
        }
        if (blackboard.Temporal.TAAHistory.IsValid())
        {
            m_SelectedHistoryTexture = blackboard.Temporal.TAAHistory;
            [[maybe_unused]] const auto taaHistoryRead = builder.Read(blackboard.Temporal.TAAHistory, RGReadUsage::ShaderSample);
        }
        if (blackboard.Post.TAAColor.IsValid())
        {
            constexpr std::string_view taaVersionTag = "TAAPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.TAAColor, RGWriteUsage::RenderTarget, taaVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::TAAColorTexture) + "@" +
                                                            std::string(taaVersionTag),
                                                        outputHandle,
                                                        0u));
            builder.ExtractHistoryTexture(ResourceNames::TAAHistory, outputHandle);
        }
    }

    void TAARenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_TAAShader = Shader::Create("assets/shaders/PostProcess_TAA.glsl");
        m_TAAUBO = UniformBuffer::Create(TAAUBOData::GetSize(), ShaderBindingLayout::UBO_TAA);

        OLO_CORE_INFO("TAARenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void TAARenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("TAARenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void TAARenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        RHI::ResourceHandle sceneDepthTextureID{};
        RHI::ResourceHandle velocityTextureID{};
        RHI::ResourceHandle historyTextureID{};
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthTextureID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedVelocityTexture.IsValid())
            velocityTextureID = context.ResolveTextureHandle(m_SelectedVelocityTexture);
        if (m_SelectedHistoryTexture.IsValid())
            historyTextureID = context.ResolveTextureHandle(m_SelectedHistoryTexture);
        RHI::ResourceHandle surfaceHistoryTextureID{};
        if (m_SelectedSurfaceHistoryTexture.IsValid())
            surfaceHistoryTextureID = context.ResolveTextureHandle(m_SelectedSurfaceHistoryTexture);
        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !m_TAAShader || !m_TAAUBO)
        {
            m_Target = nullptr;
            return;
        }
        if (!sceneDepthTextureID.IsValid())
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
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_TAAShader->Bind();

        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);
        m_TAAShader->SetInt("u_Current", 0);

        const RHI::ResourceHandle historyID = historyTextureID.IsValid() ? historyTextureID : inputColorTextureID;
        context.BindTextureOrHeapOffset(1, historyID, RHI::HeapSlotLifetime::FrameTransient);
        m_TAAShader->SetInt("u_History", 1);

        context.BindTextureOrHeapOffset(2, velocityTextureID, RHI::HeapSlotLifetime::FrameTransient);
        m_TAAShader->SetInt("u_Velocity", 2);

        // Slot 3 — last frame's RT3. Falls back to THIS frame's velocity when
        // the plane is absent (first frame, or a resize that invalidated it).
        // The shader gates on u_HasSurfaceHistory so the fallback is never read
        // as history; binding something valid just keeps the sampler defined.
        const bool hasSurfaceHistory = surfaceHistoryTextureID.IsValid();
        context.BindTextureOrHeapOffset(3, hasSurfaceHistory ? surfaceHistoryTextureID : velocityTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        m_TAAShader->SetInt("u_PrevSurface", 3);

        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        m_TAAShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

        TAAUBOData taaData;
        taaData.FeedbackSharpnessHasVelocity = glm::vec4(
            m_Settings.TAAFeedback,
            m_Settings.TAASharpness,
            velocityTextureID.IsValid() ? 1.0f : 0.0f,
            hasSurfaceHistory ? 1.0f : 0.0f);
        taaData.TexelSize = glm::vec4(
            1.0f / static_cast<f32>(outSpec.Width),
            1.0f / static_cast<f32>(outSpec.Height),
            0.0f,
            0.0f);
        m_TAAUBO->SetData(&taaData, TAAUBOData::GetSize());
        m_TAAUBO->Bind();

        // Publish the heap offsets recorded above (no-op with the heap off).
        context.FlushHeapOffsets();

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void TAARenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void TAARenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void TAARenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
        m_SelectedVelocityTexture = {};
        m_SelectedHistoryTexture = {};
        m_SelectedSurfaceHistoryTexture = {};
    }
} // namespace OloEngine
