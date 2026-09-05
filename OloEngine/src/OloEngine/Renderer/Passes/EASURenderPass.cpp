#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/PreparedFullscreenPass.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace OloEngine
{
    EASURenderPass::EASURenderPass()
    {
        SetName("EASUPass");
    }

    void EASURenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // EASU consumes the freshest pre-Bloom HDR colour — the same candidate
        // chain BloomPass reads — but at REDUCED resolution (the scene band
        // rendered into the [0, bounds] corner). It upscales that to display res,
        // so it must run at FULL viewport: it never participates in render-scale.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                // ONLY pre-EASU (reduced-resolution) sources — deliberately NOT
                // PostProcessColor, which aliases EASUColor once EASU runs and
                // would make EASU read its own output.
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ContactShadowColor, ResourceNames::ContactShadowColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSRColor, ResourceNames::SSRColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSGIColor, ResourceNames::SSGIColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (!m_Enabled)
            return;

        if (blackboard.Post.EASUColor.IsValid())
        {
            constexpr std::string_view easuVersionTag = "EASUPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.EASUColor, RGWriteUsage::RenderTarget, easuVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::EASUColorTexture) + "@" +
                                                            std::string(easuVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void EASURenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        CreateFramebuffer(spec.Width, spec.Height);

        m_EASUShader = Shader::Create("assets/shaders/PostProcess_EASU.glsl");
        m_EASUUBO = UniformBuffer::Create(EASUUBOData::GetSize(), ShaderBindingLayout::UBO_EASU);

        OLO_CORE_INFO("EASURenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void EASURenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        // EASUColor is graph-owned (declared in PopulateBlackboard, resolved per
        // frame in Execute). This pass never allocates one; m_Target stays null.
        if (width == 0 || height == 0)
            OLO_CORE_WARN("EASURenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
        m_Target = nullptr;
    }

    void EASURenderPass::Execute(RGCommandContext& context)
    {
        auto prepared = PrepareParallelRecording(context);
        if (prepared.Record)
            prepared.Record(context);
    }

    RGPreparedPass EASURenderPass::PrepareParallelRecording(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not resolved
        // here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }

        if (!m_Enabled || !inputColorTextureID.IsValid() || !outputFramebuffer || !m_EASUShader || !m_EASUUBO)
        {
            m_Target = nullptr;
            return {};
        }

        m_Target = outputFramebuffer;

        // EASU always renders at full display resolution (it is the upscale): the
        // output target carries no render-viewport override.
        const auto& outSpec = outputFramebuffer->GetSpecification();
        const auto outW = outSpec.Width;
        const auto outH = outSpec.Height;
        const f32 scale = std::clamp(m_RenderScale, 0.25f, 1.0f);
        const auto renderW = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(outW) * scale)));
        const auto renderH = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(outH) * scale)));

        EASUUBOData easuData;
        easuData.InputSizeAndTexel = glm::vec4(
            static_cast<f32>(renderW),
            static_cast<f32>(renderH),
            1.0f / static_cast<f32>(renderW),
            1.0f / static_cast<f32>(renderH));
        easuData.SampleBounds = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        m_EASUUBO->SetData(&easuData, EASUUBOData::GetSize());
        return PrepareFullscreenPass(outputFramebuffer, m_EASUShader,
                                     { { 0, inputColorTextureID, "u_Texture" } }, { m_EASUUBO });
    }

    void EASURenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void EASURenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void EASURenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
