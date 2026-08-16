#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ColorBlindRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    ColorBlindRenderPass::ColorBlindRenderPass()
    {
        SetName("ColorBlindPass");
    }

    void ColorBlindRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // The candidate list is FinalRenderPass's, because this pass sits in the
        // slot Final used to read from. UIComposite is first: when UI compositing
        // is on, that is the only image containing the HUD, and adapting the HUD
        // is the point of running here rather than beside Vignette. Every lower
        // entry is the fallback chain for a frame where UIComposite (or a later
        // stage) is disabled — drop one and the adaptation silently stops
        // applying in that configuration.
        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::UIComposite, ResourceNames::UICompositeTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::OverdrawColor, ResourceNames::OverdrawColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SelectionOutlineColor, ResourceNames::SelectionOutlineColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FXAAColor, ResourceNames::FXAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::VignetteColor, ResourceNames::VignetteColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::UpscalerColor, ResourceNames::UpscalerColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ToneMapColor, ResourceNames::ToneMapColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ColorGradingColor, ResourceNames::ColorGradingColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ChromAbColor, ResourceNames::ChromAbColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FogColor, ResourceNames::FogColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::CloudsColor, ResourceNames::CloudsColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (!m_Enabled)
            return;

        if (blackboard.Post.ColorBlindColor.IsValid())
        {
            constexpr std::string_view colorBlindVersionTag = "ColorBlindPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.ColorBlindColor, RGWriteUsage::RenderTarget, colorBlindVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::ColorBlindColorTexture) + "@" +
                                                            std::string(colorBlindVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void ColorBlindRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_ColorBlind.glsl");
        m_ParamsUBO = UniformBuffer::Create(ColorBlindUBOData::GetSize(), ShaderBindingLayout::UBO_COLORBLIND);

        OLO_CORE_INFO("ColorBlindRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ColorBlindRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        // The output (ColorBlindColor) is graph-owned: declared in
        // PopulateBlackboard, resolved per-frame in Execute. This pass never
        // allocates one, so m_Target stays null until Execute sets it.
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("ColorBlindRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void ColorBlindRenderPass::Execute(RGCommandContext& context)
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
            if (auto fb = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = fb;
        }

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !m_Shader || !m_ParamsUBO)
        {
            // This pass is TERMINAL: FinalPass reads ColorBlindColor at TOP
            // priority, so once the resource is declared, bailing out without
            // writing it presents an UNINITIALISED transient — a garbage frame,
            // not a fallback to the unadapted image. (VignettePass has the same
            // shape but is mid-chain, where a downstream stage still overwrites.)
            //
            // So if the target resolved, leave it defined even when the input or
            // the shader did not. Black is wrong, but it is deterministic and
            // legible as a failure; pooled garbage is neither.
            if (outputFramebuffer)
            {
                outputFramebuffer->Bind();
                const auto& spec = outputFramebuffer->GetSpecification();
                context.SetViewport(0, 0, spec.Width, spec.Height);
                context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
                context.Clear();
                outputFramebuffer->Unbind();
            }

            // Throttled: this runs per frame, and an unresolvable input is a
            // persistent graph condition rather than a one-off.
            static u32 s_SkipLogCount = 0;
            if (s_SkipLogCount < 8)
            {
                OLO_CORE_WARN("ColorBlindRenderPass: skipping (input valid={}, target valid={}, shader={}, ubo={})",
                              inputColorTextureID.IsValid(), static_cast<bool>(outputFramebuffer),
                              static_cast<bool>(m_Shader), static_cast<bool>(m_ParamsUBO));
                ++s_SkipLogCount;
            }

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

        // Shader BEFORE the texture: BindTextureOrHeapOffset consults
        // Shader::IsBoundProgramBindless() to choose between staging a heap
        // offset and issuing a real bind, and that flag describes the program in
        // flight (docs/agent-rules/glsl-shaders.md §5b).
        m_Shader->Bind();

        // FrameTransient: the composited colour is graph-owned.
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);

        m_ParamsUBO->SetData(&m_Params, ColorBlindUBOData::GetSize());
        m_ParamsUBO->Bind();

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.FlushHeapOffsets();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void ColorBlindRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ColorBlindRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ColorBlindRenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
