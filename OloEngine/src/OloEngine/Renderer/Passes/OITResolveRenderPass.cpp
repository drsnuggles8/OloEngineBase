#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <span>

namespace OloEngine
{
    OITResolveRenderPass::OITResolveRenderPass()
    {
        SetName("OITResolvePass");
    }

    void OITResolveRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_OITBuffer = OITBuffer::Create(spec.Width, spec.Height);
        m_ResolveShader = Shader::Create("assets/shaders/OIT_Resolve.glsl");

        OLO_CORE_INFO("OITResolveRenderPass: initialised at {}x{}", spec.Width, spec.Height);
    }

    void OITResolveRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void OITResolveRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Grab-and-reset the "has accumulation" flag so a skipped frame
        // cannot leak content from the previous one.
        const bool hasAccum = m_HasAccumulation;
        m_HasAccumulation = false;
        // Always re-arm the OITBuffer's per-frame clear guard so the next
        // frame's first transparent pass performs a fresh clear — even when
        // we early-out (disabled / no accum / missing FB). Otherwise stale
        // accumulation from a previously-OIT-enabled frame would leak if OIT
        // toggles on mid-session.
        if (m_OITBuffer)
            m_OITBuffer->ResetClearFlag();
        if (!m_Enabled || !hasAccum || !m_InputFramebuffer || !m_OITBuffer || !m_ResolveShader)
        {
            return;
        }

        // Composite into the input (scene) framebuffer's colour attachment 0.
        m_InputFramebuffer->Bind();

        const auto& spec = m_InputFramebuffer->GetSpecification();
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
        RenderCommand::SetBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);

        // Don't touch entity ID or view-normals while compositing transparents.
        RenderCommand::SetColorMaskForAttachment(1, false, false, false, false);
        RenderCommand::SetColorMaskForAttachment(2, false, false, false, false);

        m_ResolveShader->Bind();
        context.BindTexture(ShaderBindingLayout::TEX_OIT_ACCUM, m_OITBuffer->GetAccumAttachmentID());
        context.BindTexture(ShaderBindingLayout::TEX_OIT_REVEALAGE, m_OITBuffer->GetRevealageAttachmentID());

        DrawFullscreenTriangle(context);

        // Restore mutable GL state for subsequent passes.
        RenderCommand::SetColorMaskForAttachment(1, true, true, true, true);
        RenderCommand::SetColorMaskForAttachment(2, true, true, true, true);
        RenderCommand::SetBlendStateForAttachment(0, false);
        context.SetBlendState(false);
        context.SetDepthMask(true);
        context.SetDepthTest(true);

        // Restore the full MRT draw-buffer set we narrowed above so the next
        // pass binding this framebuffer writes to all attachments again. Use
        // the actual attachment count from the framebuffer spec rather than
        // hard-coding 3, so deferred / forward FBs (which have different
        // colour-attachment counts) all restore correctly.
        const auto& inputAttachments = m_InputFramebuffer->GetSpecification().Attachments.Attachments;
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

        m_InputFramebuffer->Unbind();
    }

    Ref<Framebuffer> OITResolveRenderPass::GetTarget() const
    {
        // Passthrough: downstream passes always read the scene FB, composited or not.
        return m_InputFramebuffer;
    }

    void OITResolveRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        if (m_OITBuffer)
            m_OITBuffer->Resize(width, height);
    }

    void OITResolveRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        if (m_OITBuffer)
            m_OITBuffer->Resize(width, height);
    }

    void OITResolveRenderPass::OnReset()
    {
        m_HasAccumulation = false;
    }

    void OITResolveRenderPass::DrawFullscreenTriangle(RGCommandContext& context)
    {
        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);
    }
} // namespace OloEngine
