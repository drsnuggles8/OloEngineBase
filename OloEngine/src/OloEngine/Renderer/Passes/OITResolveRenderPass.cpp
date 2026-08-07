#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"

#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <array>
#include <span>

namespace OloEngine
{
    OITResolveRenderPass::OITResolveRenderPass()
    {
        SetName("OITResolvePass");
    }

    void OITResolveRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedOITAccumTexture = {};
        m_SelectedOITRevealageTexture = {};

        const auto inputHandle = RenderPipelineBuilderInternal::SelectFirstValidFramebufferForPass(this, blackboard.Scene.SceneColor);

        if (!m_Enabled || !m_HasContributors)
            return;

        if (blackboard.OIT.OITAccum.IsValid())
        {
            m_SelectedOITAccumTexture = blackboard.OIT.OITAccum;
            // Resolve samples OITAccum / OITRevealage as textures (see Execute
            // BindTexture calls), not as input attachments — the hazard planner
            // needs a sample barrier, not a sub-pass attachment-read.
            [[maybe_unused]] const auto oitAccumRead = builder.Read(blackboard.OIT.OITAccum, RGReadUsage::ShaderSample);
        }
        if (blackboard.OIT.OITRevealage.IsValid())
        {
            m_SelectedOITRevealageTexture = blackboard.OIT.OITRevealage;
            [[maybe_unused]] const auto oitRevealageRead = builder.Read(blackboard.OIT.OITRevealage, RGReadUsage::ShaderSample);
        }

        // Inter-pass RMW: read the prior SceneColor version (the input
        // framebuffer selected above) and advertise a renamed output via
        // WriteNewVersion. The prior-version RenderTargetRead is emitted by
        // ReadFirstValidFramebuffer below; the rename means no same-pass
        // feedback loop exists for the validator. `WriteNewVersion`
        // republishes the base attachment views as versioned siblings; see
        // ForwardOverlayRenderPass for the rationale.
        RenderPipelineBuilderInternal::ReadFirstValidFramebuffer(builder, inputHandle);
        if (blackboard.Scene.SceneColor.IsValid())
        {
            constexpr std::string_view oitResolveVersionTag = "OITResolvePass";
            [[maybe_unused]] const auto sceneColorNew =
                builder.WriteNewVersion(blackboard.Scene.SceneColor, RGWriteUsage::RenderTarget, oitResolveVersionTag);
            builder.DependsOnPreviousWriter(ResourceNames::SceneColor);
        }
    }

    void OITResolveRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_ResolveShader = Shader::Create("assets/shaders/OIT_Resolve.glsl");

        OLO_CORE_INFO("OITResolveRenderPass: initialised at {}x{}", spec.Width, spec.Height);
    }

    void OITResolveRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        Ref<Framebuffer> inputFB;
        RHI::ResourceHandle oitAccumTextureID{};
        RHI::ResourceHandle oitRevealageTextureID{};
        if (const auto inputHandle = GetPrimaryInputFramebufferHandle(); inputHandle.IsValid())
        {
            if (auto resolvedInput = context.ResolveFramebuffer(inputHandle))
                inputFB = resolvedInput;
        }

        if (m_SelectedOITAccumTexture.IsValid())
            oitAccumTextureID = context.ResolveTextureHandle(m_SelectedOITAccumTexture);
        if (m_SelectedOITRevealageTexture.IsValid())
            oitRevealageTextureID = context.ResolveTextureHandle(m_SelectedOITRevealageTexture);

        if (!m_Enabled || !inputFB || !oitAccumTextureID.IsValid() || !oitRevealageTextureID.IsValid() || !m_ResolveShader)
        {
            return;
        }

        // Composite into the input (scene) framebuffer's colour attachment 0.
        inputFB->Bind();

        const auto& spec = inputFB->GetSpecification();
        context.SetViewport(0, 0, spec.Width, spec.Height);

        // Restrict the draw-buffer set to COLOR_ATTACHMENT0 so the fullscreen
        // fragment shader cannot accidentally clobber entity-ID / view-normal
        // attachments — colour-mask writes still leave an undefined-output
        // hazard on some drivers when MRT is enabled. Goes through
        // RenderCommand so MockRendererAPI sees the change in tests.
        const u32 oitResolveDrawAttachment = 0u;
        context.SetDrawBuffers(std::span<const u32>(&oitResolveDrawAttachment, 1));

        // No depth interaction: the accum already baked in depth-weighting.
        context.SetDepthTest(false);
        context.SetDepthMask(false);

        // Blend equation:
        //   dst' = averageColor * (1 - revealage) + dst * revealage
        // Shader outputs o_Color = (averageColor.rgb, revealage) and we use
        //   src = GL_ONE_MINUS_SRC_ALPHA, dst = GL_SRC_ALPHA.
        RenderCommand::SetBlendStateForAttachment(0, true);
        RenderCommand::SetBlendStateForAttachment(1, false); // entity ID — integer
        RenderCommand::SetBlendStateForAttachment(2, false); // view normals
        RenderCommand::SetBlendFunc(RHI::BlendFactor::OneMinusSrcAlpha, RHI::BlendFactor::SrcAlpha);

        // Don't touch entity ID or view-normals while compositing transparents.
        RenderCommand::SetColorMaskForAttachment(1, false, false, false, false);
        RenderCommand::SetColorMaskForAttachment(2, false, false, false, false);

        // Heap-bindless conversion (issue #691 Phase 3, bucket 1). Shader first —
        // the seam forks on the program in flight. Both inputs are graph-resolved
        // (pooled), hence FrameTransient. DrawFullscreenTriangle flushes.
        m_ResolveShader->Bind();
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_OIT_ACCUM, oitAccumTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_OIT_REVEALAGE, oitRevealageTextureID,
                                        RHI::HeapSlotLifetime::FrameTransient);

        DrawFullscreenTriangle(context);

        // Restore mutable GL state for subsequent passes.
        RenderCommand::SetColorMaskForAttachment(1, true, true, true, true);
        RenderCommand::SetColorMaskForAttachment(2, true, true, true, true);
        RenderCommand::SetBlendStateForAttachment(0, false);
        context.SetBlendState(false);
        // Clearing the inputs needs the seam AND its own flush. Under the heap
        // there is no bind to undo — the shader reads an offset — so an offset
        // left in the table keeps a later pass sampling this frame's accum
        // buffer through a perfectly valid index. The seam stages the reserved
        // null; without the flush that clear would only land whenever some other
        // pass happened to flush next (§4b, "unbind has no translation").
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_OIT_ACCUM, RHI::NullResource,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_OIT_REVEALAGE, RHI::NullResource,
                                        RHI::HeapSlotLifetime::FrameTransient);
        context.FlushHeapOffsets();
        context.SetDepthMask(true);
        context.SetDepthTest(true);

        // Restore the full MRT draw-buffer set we narrowed above so the next
        // pass binding this framebuffer writes to all attachments again. Use
        // the actual attachment count from the framebuffer spec rather than
        // hard-coding 3, so deferred / forward FBs (which have different
        // colour-attachment counts) all restore correctly.
        const auto& inputAttachments = inputFB->GetSpecification().Attachments.Attachments;
        u32 colorAttachmentCount = 0;
        for (const auto& att : inputAttachments)
        {
            if (att.TextureFormat != FramebufferTextureFormat::Depth &&
                att.TextureFormat != FramebufferTextureFormat::DEPTH24STENCIL8 &&
                att.TextureFormat != FramebufferTextureFormat::DEPTH_COMPONENT32F &&
                att.TextureFormat != FramebufferTextureFormat::ShadowDepth)
            {
                ++colorAttachmentCount;
            }
        }
        if (colorAttachmentCount > 0)
        {
            std::array<u32, 8> restoreAttachments{};
            const u32 count = std::min<u32>(colorAttachmentCount, static_cast<u32>(restoreAttachments.size()));
            for (u32 i = 0; i < count; ++i)
                restoreAttachments[i] = i;
            context.SetDrawBuffers(std::span<const u32>(restoreAttachments.data(), count));
        }

        inputFB->Unbind();
    }

    void OITResolveRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void OITResolveRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void OITResolveRenderPass::OnReset()
    {
        m_SelectedOITAccumTexture = {};
        m_SelectedOITRevealageTexture = {};
    }

    void OITResolveRenderPass::DrawFullscreenTriangle(RGCommandContext& context) const
    {
        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        // THE FLUSH LIVES WITH THE DRAW, not at the call site. "A flush per draw"
        // (§4b) is a rule someone has to remember at every call site; putting it
        // in the one helper that issues this pass's draw makes it structural
        // instead. Publishes the descriptors minted since the last flush AND the
        // offsets indexing them, in that order (issue #691 Phase 3).
        context.FlushHeapOffsets();
        context.DrawIndexed(va);
    }
} // namespace OloEngine
