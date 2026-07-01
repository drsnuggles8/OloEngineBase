#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/EASURenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

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
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ContactShadowColor, ResourceNames::ContactShadowColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSRColor, ResourceNames::SSRColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
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
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not resolved
        // here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }

        if (!m_Enabled || inputColorTextureID == 0u || !outputFramebuffer || !m_EASUShader || !m_EASUUBO)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        // EASU always renders at full display resolution (it is the upscale): the
        // output target carries no render-viewport override.
        outputFramebuffer->Bind();

        const auto& outSpec = outputFramebuffer->GetSpecification();
        const auto outW = outSpec.Width;
        const auto outH = outSpec.Height;
        context.SetViewport(0, 0, outW, outH);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_EASUShader->Bind();

        context.BindTexture(0, inputColorTextureID);
        m_EASUShader->SetInt("u_Texture", 0);

        // The input is a genuinely reduced-size scene target of floor(physical *
        // scale) — MUST match the reduced dimensions PopulateBlackboard sizes the
        // scene band at. EASU samples that whole texture (bounds = 1.0): output UV
        // maps into the reduced pixel grid (u_RenderSize) and taps use the input's
        // own texel size (1 / reduced), reconstructing display res.
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
        m_EASUUBO->Bind();

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
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
