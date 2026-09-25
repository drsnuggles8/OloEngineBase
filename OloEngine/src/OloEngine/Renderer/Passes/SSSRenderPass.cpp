#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <array>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // Scene colour only: attachment 4 — the hand-off this pass READS — and
        // every other attachment stay untouched.
        constexpr std::array<u32, 1> kAttachment0Only = { 0u };
    } // namespace

    SSSRenderPass::SSSRenderPass()
    {
        SetName("SSSPass");
    }

    void SSSRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_HandoffTexture = {};
        m_SceneDepthTexture = {};
        m_SceneColorFramebuffer = {};

        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled)
            return;

        // Every input is required; a blur with no depth cannot tell a snowbank
        // from the wall behind it, so without one the pass claims nothing and
        // the graph culls it.
        if (!blackboard.Scene.SkinDiffuse.IsValid() || !blackboard.Scene.SceneDepthAttachment.IsValid() ||
            !blackboard.Scene.SceneColor.IsValid())
        {
            return;
        }

        m_HandoffTexture = blackboard.Scene.SkinDiffuse;
        m_SceneDepthTexture = blackboard.Scene.SceneDepthAttachment;
        [[maybe_unused]] const auto handoffRead = builder.Read(m_HandoffTexture, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto depthRead = builder.Read(m_SceneDepthTexture, RGReadUsage::ShaderSample);

        // Scene colour is written IN PLACE by an additive blend, exactly as
        // SkinDiffusionPass writes it: the graph is told about the write for
        // ordering and barriers, and the declared same-framebuffer read/write
        // covers the disjoint subresources — this pass draws attachment 0 ONLY
        // and samples attachment 4 and depth, which nothing in it writes.
        m_SceneColorFramebuffer = blackboard.Scene.SceneColor;
        SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
        builder.Write(blackboard.Scene.SceneColor, RGWriteUsage::RenderTarget);
        builder.AllowSamePassReadWrite(blackboard.Scene.SceneColor);
    }

    void SSSRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_SSSBlurShader = Shader::Create("assets/shaders/SSS_Blur.glsl");
        OLO_CORE_INFO("SSSRenderPass: Initialized");
    }

    void SSSRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;
        if (!m_Settings.Enabled || !m_Settings.SSSBlurEnabled || !IsReadyForExecution())
            return;
        if (!m_HandoffTexture.IsValid() || !m_SceneDepthTexture.IsValid() || !m_SceneColorFramebuffer.IsValid())
            return;

        const RHI::ResourceHandle handoffID = context.ResolveTextureHandle(m_HandoffTexture);
        const RHI::ResourceHandle depthID = context.ResolveTextureHandle(m_SceneDepthTexture);
        Ref<Framebuffer> sceneFB = context.ResolveFramebuffer(m_SceneColorFramebuffer);
        if (!handoffID.IsValid() || !depthID.IsValid() || !sceneFB)
            return;

        const auto& sceneSpec = sceneFB->GetSpecification();
        const u32 width = sceneSpec.Width;
        const u32 height = sceneSpec.Height;
        if (width == 0 || height == 0)
            return;

        // The UBO: radius and depth falloff as authored, the target size, and
        // the scatter strength (the fraction of the snow's diffuse light that
        // is redistributed). Non-finite authored values fall back to defaults.
        SSSUBOData data = m_GPUData ? *m_GPUData : SSSUBOData{};
        const f32 radius = std::isfinite(m_Settings.SSSBlurRadius) ? std::max(m_Settings.SSSBlurRadius, 0.0f) : 2.0f;
        const f32 falloff = std::isfinite(m_Settings.SSSBlurFalloff) ? std::max(m_Settings.SSSBlurFalloff, 0.0f) : 1.0f;
        const f32 strength =
            std::isfinite(m_Settings.SSSIntensity) ? std::clamp(m_Settings.SSSIntensity, 0.0f, 1.0f) : 0.6f;
        data.BlurParams = glm::vec4(radius, falloff, static_cast<f32>(width), static_cast<f32>(height));
        data.Flags = glm::vec4(1.0f, strength, 0.0f, 0.0f);
        if (m_GPUData)
            *m_GPUData = data;
        if (!m_SSSUBO)
            return;
        m_SSSUBO->SetData(&data, SSSUBOData::GetSize());
        m_SSSUBO->Bind();

        // No GLStateGuard: a per-frame pass restores what it changed explicitly
        // (SkinDiffusionPass states why a guard is the wrong tool here).
        auto va = MeshPrimitives::GetFullscreenTriangle();
        sceneFB->Bind();
        RenderCommand::SetFramebufferDrawAttachments(sceneFB->GetRHIHandle(), kAttachment0Only);
        // Open the mask this pass writes through (docs/agent-rules/a-pass-opens-its-own-colour-mask.md):
        // the draw before it may have left attachment 0 masked, and the blur
        // would then add nothing.
        RenderCommand::SetColorMask(true, true, true, true);
        context.SetViewport(0, 0, width, height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetCulling(false);
        context.SetBlendState(true);
        // ONE/ONE on colour: the shader emits a signed delta, zero off snow.
        // ZERO/ONE on alpha: scene alpha is the blend alpha of whatever drew
        // there and is not this pass's to change.
        RenderCommand::SetBlendFuncSeparate(RHI::BlendFactor::One, RHI::BlendFactor::One, RHI::BlendFactor::Zero,
                                            RHI::BlendFactor::One);
        RenderCommand::SetBlendEquation(RHI::BlendOp::Add);

        m_SSSBlurShader->Bind();
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE, handoffID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID,
                                         RHI::HeapSlotLifetime::FrameTransient);
        va->Bind();
        HeapBinding::FlushOffsets();
        RenderCommand::DrawIndexed(va);

        // Restore the scene framebuffer's full draw-buffer set, the default
        // blend function and the depth mask — the same restore SkinDiffusionPass
        // performs, for the same reasons.
        u32 colorCount = 0;
        for (const auto& attachment : sceneSpec.Attachments.Attachments)
        {
            const bool isDepth = (attachment.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  attachment.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && attachment.TextureFormat != FramebufferTextureFormat::None)
                ++colorCount;
        }
        RenderCommand::RestoreAllFramebufferDrawAttachments(sceneFB->GetRHIHandle(), colorCount);
        RenderCommand::SetBlendFuncSeparate(RHI::BlendFactor::One, RHI::BlendFactor::Zero, RHI::BlendFactor::One,
                                            RHI::BlendFactor::Zero);
        RenderCommand::SetBlendEquation(RHI::BlendOp::Add);
        context.SetBlendState(false);
        context.SetDepthMask(true);

        m_Target = sceneFB;
    }

    Ref<Framebuffer> SSSRenderPass::GetTarget() const
    {
        return (m_Settings.Enabled && m_Settings.SSSBlurEnabled) ? m_Target : nullptr;
    }

    void SSSRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSSRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void SSSRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_HandoffTexture = {};
        m_SceneDepthTexture = {};
        m_SceneColorFramebuffer = {};
    }
} // namespace OloEngine
