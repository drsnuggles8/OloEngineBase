#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/CloudscapeRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    CloudscapeRenderPass::CloudscapeRenderPass()
    {
        SetName("CloudscapePass");
    }

    void CloudscapeRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedCloudsRawFramebuffer = {};
        m_SelectedCloudsResolvedFramebuffer = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedHistoryTexture = {};

        // Clouds run AFTER TAA (they consume the anti-aliased colour) and
        // BEFORE Precipitation, so the candidate ladder must not list
        // PrecipitationColor.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
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

        // Prior-frame resolved clouds, imported by Renderer3D only when the
        // previous frame's history copy-back succeeded.
        if (blackboard.Temporal.CloudsHistory.IsValid())
        {
            m_SelectedHistoryTexture = blackboard.Temporal.CloudsHistory;
            [[maybe_unused]] const auto cloudsHistoryRead = builder.Read(blackboard.Temporal.CloudsHistory, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.CloudsRaw.IsValid())
        {
            m_SelectedCloudsRawFramebuffer = blackboard.Scratch.CloudsRaw;
            // Intra-pass write-then-sample: draw A renders the half-res
            // raymarch into CloudsRaw; draw B (temporal resolve) samples that
            // result inside the same Execute. Graph-owned scratch with no
            // prior writer to chain against (same idiom as Fog's FogHalfRes).
            builder.AllowSamePassReadWrite(blackboard.Scratch.CloudsRaw);
            builder.Write(blackboard.Scratch.CloudsRaw, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto cloudsRawRead = builder.Read(blackboard.Scratch.CloudsRaw, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.CloudsResolved.IsValid())
        {
            m_SelectedCloudsResolvedFramebuffer = blackboard.Scratch.CloudsResolved;
            // Intra-pass write-then-sample again: draw B writes the resolve,
            // draw C (full-res composite) samples it.
            builder.AllowSamePassReadWrite(blackboard.Scratch.CloudsResolved);
            builder.Write(blackboard.Scratch.CloudsResolved, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto cloudsResolvedRead = builder.Read(blackboard.Scratch.CloudsResolved, RGReadUsage::ShaderSample);
            // Next-frame temporal history: the graph copies the resolved
            // buffer into the pipeline-owned CloudsHistory sink after this
            // pass executes (TAA's RegisterHistoryTextureSink /
            // ExtractHistoryTexture pattern, at half resolution).
            builder.ExtractHistoryTexture(ResourceNames::CloudsHistory, blackboard.Scratch.CloudsResolved);
        }

        if (blackboard.Post.CloudsColor.IsValid())
        {
            constexpr std::string_view cloudsVersionTag = "CloudscapePass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.CloudsColor, RGWriteUsage::RenderTarget, cloudsVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::CloudsColorTexture) + "@" +
                                                            std::string(cloudsVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void CloudscapeRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_RaymarchShader = Shader::Create("assets/shaders/PostProcess_Cloudscape.glsl");
        m_ResolveShader = Shader::Create("assets/shaders/PostProcess_CloudscapeResolve.glsl");
        m_CompositeShader = Shader::Create("assets/shaders/PostProcess_CloudscapeComposite.glsl");

        m_CloudscapeUBO = UniformBuffer::Create(UBOStructures::CloudscapeUBO::GetSize(),
                                                ShaderBindingLayout::UBO_CLOUDSCAPE);

        OLO_CORE_INFO("CloudscapeRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void CloudscapeRenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("CloudscapeRenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        // Graph-owned framebuffers only (CloudsRaw / CloudsResolved /
        // CloudsColor are declared by Renderer3D and resolved in Execute).
        m_Target = nullptr;
    }

    void CloudscapeRenderPass::UploadAndBindUBO()
    {
        if (!m_CloudscapeUBO)
            return;

        UBOStructures::CloudscapeUBO data = m_UBOData;
        // First frame / post-invalidation: no history to blend against —
        // force the temporal-blend lane to 0 so the resolve outputs the
        // current frame (the shader gates on u_CloudMisc.x). m_HistoryValid
        // reflects the post-PopulateBlackboard state, which is authoritative
        // over the snapshot filled in ConfigurePassesForFrame.
        if (!m_HistoryValid)
            data.Misc.x = 0.0f;
        m_CloudscapeUBO->SetData(&data, UBOStructures::CloudscapeUBO::GetSize());
        m_CloudscapeUBO->Bind();
    }

    void CloudscapeRenderPass::UploadDisabledUBO()
    {
        if (!m_CloudscapeUBO)
            return;

        UBOStructures::CloudscapeUBO disabled{};
        disabled.Misc = glm::vec4(0.0f); // w = 0 -> cloudscape disabled, shaders early-out
        m_CloudscapeUBO->SetData(&disabled, UBOStructures::CloudscapeUBO::GetSize());
        m_CloudscapeUBO->Bind();
    }

    void CloudscapeRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        Ref<Framebuffer> cloudsRawFramebuffer;
        Ref<Framebuffer> cloudsResolvedFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }
        if (m_SelectedCloudsRawFramebuffer.IsValid())
            cloudsRawFramebuffer = context.ResolveFramebuffer(m_SelectedCloudsRawFramebuffer);
        if (m_SelectedCloudsResolvedFramebuffer.IsValid())
            cloudsResolvedFramebuffer = context.ResolveFramebuffer(m_SelectedCloudsResolvedFramebuffer);

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer || !cloudsRawFramebuffer || !cloudsResolvedFramebuffer ||
            !m_RaymarchShader || !m_ResolveShader || !m_CompositeShader)
        {
            m_Target = nullptr;
            return;
        }

        const u32 sceneDepthTextureID = m_SelectedSceneDepthTexture.IsValid()
                                            ? context.ResolveTexture(m_SelectedSceneDepthTexture)
                                            : 0u;
        if (sceneDepthTextureID == 0)
        {
            m_Target = nullptr;
            return; // raymarch termination + composite upsample both need depth
        }

        m_Target = outputFramebuffer;

        // Re-bind the full shared camera UBO at binding 0. All three cloud
        // shaders read the full CameraMatrices layout — u_CameraPosition
        // (std140 offset 192) and u_RenderOrigin (offset 272) — but an
        // earlier 64-byte ViewProjection-only camera UBO (Renderer2D /
        // ParticleBatchRenderer style) can be left bound at slot 0, which
        // makes those reads out-of-bounds (origin-centred scenes survive
        // only because robust-access OOB reads return 0 ≈ the true camera).
        // Pin the full 288-byte UBO here so off-origin worlds march
        // correctly (mirrors FogRenderPass's binding-0 re-bind).
        if (m_CameraUBO)
            m_CameraUBO->Bind();

        // Re-bind the CloudscapeData UBO at binding 52. Its contents were
        // uploaded pre-graph by RenderPipeline::UploadExecutionState (the
        // CloudShadow compute consumes the same UBO outside the graph); this
        // only defends the binding slot against cross-frame buffer churn.
        if (m_CloudscapeUBO)
            m_CloudscapeUBO->Bind();

        // Defensive sampler re-binds for the raymarch's noise field
        // (56/57/58 carry explicit layout(binding) qualifiers in
        // CloudscapeCommon.glsl). UploadExecutionState bound these already;
        // re-pin them in case an earlier pass touched the units.
        if (m_BaseNoiseTextureID != 0)
            context.BindTexture(ShaderBindingLayout::TEX_CLOUD_BASE_NOISE, m_BaseNoiseTextureID);
        if (m_DetailNoiseTextureID != 0)
            context.BindTexture(ShaderBindingLayout::TEX_CLOUD_DETAIL_NOISE, m_DetailNoiseTextureID);
        if (m_WeatherMapTextureID != 0)
            context.BindTexture(ShaderBindingLayout::TEX_CLOUD_WEATHER_MAP, m_WeatherMapTextureID);

        // ----------------------------------------------------------------
        // Pass A — Half-resolution raymarch.
        // Output: RGBA16F (RGB = premultiplied inscatter, A = transmittance).
        // ----------------------------------------------------------------
        cloudsRawFramebuffer->Bind();
        const auto& cloudsRawSpec = cloudsRawFramebuffer->GetSpecification();
        context.SetViewport(0, 0, cloudsRawSpec.Width, cloudsRawSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        {
            constexpr u32 colorAttachment = 0;
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f }); // a = 1: no cloud
        context.Clear();

        m_RaymarchShader->Bind();

        // Full-resolution depth (the shader samples at half-res UV).
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }
        cloudsRawFramebuffer->Unbind();

        // ----------------------------------------------------------------
        // Pass B — Half-resolution temporal resolve.
        // ----------------------------------------------------------------
        cloudsResolvedFramebuffer->Bind();
        const auto& cloudsResolvedSpec = cloudsResolvedFramebuffer->GetSpecification();
        context.SetViewport(0, 0, cloudsResolvedSpec.Width, cloudsResolvedSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        {
            constexpr u32 colorAttachment = 0;
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_ResolveShader->Bind();

        // This frame's raymarch at unit 0 (layout(binding = 0) in the shader).
        const u32 cloudsRawColorID = cloudsRawFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, cloudsRawColorID);

        // History at unit 1. Prefer the graph-imported handle (always the
        // live texture); fall back to the pipeline-supplied raw id, then to
        // the current frame when no valid history exists — with Misc.x
        // forced to 0 in that case (UploadAndBindUBO) the shader ignores it.
        u32 historyTextureID = 0u;
        if (m_SelectedHistoryTexture.IsValid())
            historyTextureID = context.ResolveTexture(m_SelectedHistoryTexture);
        if (historyTextureID == 0u)
            historyTextureID = m_HistoryTextureID;
        const u32 historyBindID = (m_HistoryValid && historyTextureID != 0u) ? historyTextureID : cloudsRawColorID;
        context.BindTexture(1, historyBindID);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }
        cloudsResolvedFramebuffer->Unbind();

        // ----------------------------------------------------------------
        // Pass C — Full-resolution depth-aware upsample + composite:
        //          result = scene * transmittance + inscatter.
        // ----------------------------------------------------------------
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
        {
            constexpr u32 colorAttachment = 0;
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        }
        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_CompositeShader->Bind();

        // Upstream scene colour at unit 0, resolved clouds at unit 1,
        // full-res depth at TEX_POSTPROCESS_DEPTH (all layout-qualified).
        context.BindTexture(0, inputColorTextureID);
        context.BindTexture(1, cloudsResolvedFramebuffer->GetColorAttachmentRendererID(0));
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthTextureID);

        {
            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            context.DrawIndexed(va);
        }

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void CloudscapeRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void CloudscapeRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void CloudscapeRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedCloudsRawFramebuffer = {};
        m_SelectedCloudsResolvedFramebuffer = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedHistoryTexture = {};
    }
} // namespace OloEngine
