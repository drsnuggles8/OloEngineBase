#include "OloEnginePCH.h"
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
            return;
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !depthID.IsValid())
        {
            m_Target = nullptr;
            return;
        }

        if (!IsReadyForExecution())
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        const auto& targetSpec = outputFramebuffer->GetSpecification();
        constexpr u32 colorAttachment = 0;

        // SSS UBO data is uploaded by Renderer3D::EndScene, but SetData()
        // doesn't refresh the indexed binding — other passes (IBL precompute,
        // Bloom mip updates) may have displaced binding 14 between EndScene
        // and this Execute. Rebind here.
        if (m_SSSUBO)
            m_SSSUBO->Bind();

        outputFramebuffer->Bind();

        context.SetViewport(0, 0, targetSpec.Width, targetSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        m_SSSBlurShader->Bind();

        // Bind input scene color as texture — no read-write hazard since the
        // input is sampled and we write to the graph-owned SSSColor target.
        // FrameTransient: graph-resolved, so the descriptor comes from the
        // per-frame ring rather than being memoised onto a pooled object the
        // planner may reassign next frame (issue #691 Phase 3).
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);

        // Bind scene depth for bilateral filtering
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID,
                                        RHI::HeapSlotLifetime::FrameTransient);

        DrawFullscreenTriangle(context);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void SSSRenderPass::DrawFullscreenTriangle(RGCommandContext& context) const
    {
        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        // The flush lives with the draw — see OITResolveRenderPass for why
        // (issue #691 Phase 3).
        context.FlushHeapOffsets();
        context.DrawIndexed(va);
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
