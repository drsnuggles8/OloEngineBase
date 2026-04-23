#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

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
        RenderCommand::SetViewport(0, 0, spec.Width, spec.Height);

        // No depth interaction: the accum already baked in depth-weighting.
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);

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
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_OIT_ACCUM,     m_OITBuffer->GetAccumAttachmentID());
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_OIT_REVEALAGE, m_OITBuffer->GetRevealageAttachmentID());

        DrawFullscreenTriangle();

        // Restore mutable GL state for subsequent passes.
        RenderCommand::SetColorMaskForAttachment(1, true, true, true, true);
        RenderCommand::SetColorMaskForAttachment(2, true, true, true, true);
        RenderCommand::SetBlendStateForAttachment(0, false);
        RenderCommand::SetBlendState(false);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthTest(true);

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

    void OITResolveRenderPass::DrawFullscreenTriangle()
    {
        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
    }
} // namespace OloEngine
