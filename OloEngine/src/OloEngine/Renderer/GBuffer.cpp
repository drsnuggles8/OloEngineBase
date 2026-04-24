#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GBuffer.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <glad/gl.h>

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        FramebufferSpecification BuildSpec(u32 width, u32 height, u32 sampleCount)
        {
            FramebufferSpecification spec;
            spec.Width = width;
            spec.Height = height;
            spec.Samples = sampleCount;
            spec.Attachments = FramebufferAttachmentSpecification{
                FramebufferTextureSpecification{ FramebufferTextureFormat::RGBA8 },   // RT0 Albedo + Metallic
                FramebufferTextureSpecification{ FramebufferTextureFormat::RGBA16F }, // RT1 Normal + Roughness + AO
                FramebufferTextureSpecification{ FramebufferTextureFormat::RGBA16F }, // RT2 Emissive + Flags
                FramebufferTextureSpecification{ FramebufferTextureFormat::RG16F },   // RT3 Velocity
                // Depth must match the scene framebuffer's depth format
                // (`FramebufferTextureFormat::Depth` = DEPTH24STENCIL8) so that
                // `glBlitNamedFramebuffer(GL_DEPTH_BUFFER_BIT, …)` — the path used
                // by `DeferredLightingPass` to hand G-Buffer depth to downstream
                // passes and by `SceneRenderPass::ResolveToScene` in forward+ —
                // succeeds. A format mismatch here surfaces as a per-frame flood
                // of `GL_INVALID_OPERATION: Depth formats do not match` and
                // leaves the scene-FB depth uninitialised, breaking every
                // downstream depth-read (overlays, foliage, decals, water,
                // SSAO/GTAO, fog, DoF, motion blur).
                FramebufferTextureSpecification{ FramebufferTextureFormat::Depth }
            };
            return spec;
        }

        // Clamp to the same upper bound Framebuffer enforces so creation
        // never fails silently on absurd inputs.
        constexpr u32 kMaxGBufferSize = 8192;
    } // namespace

    Ref<GBuffer> GBuffer::Create(u32 width, u32 height, u32 sampleCount)
    {
        OLO_PROFILE_FUNCTION();

        // The Framebuffer implementation on OpenGL asserts width/height > 0,
        // so fall back to a 1x1 placeholder that can later be resized.
        if (width == 0 || height == 0)
        {
            width = std::max<u32>(width, 1u);
            height = std::max<u32>(height, 1u);
        }
        if (width > kMaxGBufferSize || height > kMaxGBufferSize)
        {
            OLO_CORE_WARN("GBuffer::Create: size ({}x{}) exceeds max {}x{}, clamping.",
                          width, height, kMaxGBufferSize, kMaxGBufferSize);
            width = std::min(width, kMaxGBufferSize);
            height = std::min(height, kMaxGBufferSize);
        }
        if (sampleCount != 1 && sampleCount != 2 && sampleCount != 4 && sampleCount != 8)
        {
            OLO_CORE_WARN("GBuffer::Create: invalid sample count {}, forcing 1.", sampleCount);
            sampleCount = 1;
        }

        return Ref<GBuffer>(new GBuffer(width, height, sampleCount));
    }

    GBuffer::GBuffer(u32 width, u32 height, u32 sampleCount)
        : m_Width(width), m_Height(height), m_SampleCount(sampleCount)
    {
        Recreate();
    }

    void GBuffer::Recreate()
    {
        OLO_PROFILE_FUNCTION();

        m_Framebuffer = Framebuffer::Create(BuildSpec(m_Width, m_Height, m_SampleCount));
        // Single-sample resolve target mirrors the MSAA layout, used by
        // DeferredLightingPass / OITResolvePass as sampler sources.
        if (m_SampleCount > 1)
        {
            m_ResolvedFramebuffer = Framebuffer::Create(BuildSpec(m_Width, m_Height, 1));
        }
        else
        {
            m_ResolvedFramebuffer.Reset();
        }
    }

    void GBuffer::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
            return;
        if (width > kMaxGBufferSize)
            width = kMaxGBufferSize;
        if (height > kMaxGBufferSize)
            height = kMaxGBufferSize;

        if (width == m_Width && height == m_Height)
            return;

        m_Width = width;
        m_Height = height;
        if (m_Framebuffer)
            m_Framebuffer->Resize(m_Width, m_Height);
        else
            Recreate();

        if (m_ResolvedFramebuffer)
            m_ResolvedFramebuffer->Resize(m_Width, m_Height);
    }

    void GBuffer::Resolve()
    {
        OLO_PROFILE_FUNCTION();

        if (m_SampleCount <= 1 || !m_Framebuffer || !m_ResolvedFramebuffer)
            return;

        const u32 srcFB = m_Framebuffer->GetRendererID();
        const u32 dstFB = m_ResolvedFramebuffer->GetRendererID();
        const GLint w = static_cast<GLint>(m_Width);
        const GLint h = static_cast<GLint>(m_Height);

        // Resolve each colour attachment independently — glBlitNamedFramebuffer
        // only reads/writes the currently-selected read-/draw-buffer so this
        // is the safe pattern for MRT MSAA resolve.
        for (u32 i = 0; i < static_cast<u32>(Count); ++i)
        {
            const GLenum attachment = GL_COLOR_ATTACHMENT0 + i;
            glNamedFramebufferReadBuffer(srcFB, attachment);
            glNamedFramebufferDrawBuffer(dstFB, attachment);
            glBlitNamedFramebuffer(srcFB, dstFB,
                                   0, 0, w, h,
                                   0, 0, w, h,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        // Resolve depth (no sample filtering — GL_NEAREST is the only legal choice).
        glBlitNamedFramebuffer(srcFB, dstFB,
                               0, 0, w, h,
                               0, 0, w, h,
                               GL_DEPTH_BUFFER_BIT, GL_NEAREST);

        // Restore full draw-buffer set on the resolved FB so subsequent
        // passes that bind it for composition get all attachments.
        const GLenum fullDrawBufs[] = {
            GL_COLOR_ATTACHMENT0,
            GL_COLOR_ATTACHMENT1,
            GL_COLOR_ATTACHMENT2,
            GL_COLOR_ATTACHMENT3
        };
        glNamedFramebufferDrawBuffers(dstFB, 4, fullDrawBufs);
    }

    u32 GBuffer::GetColorAttachmentID(AttachmentIndex index) const
    {
        const auto& fb = m_ResolvedFramebuffer ? m_ResolvedFramebuffer : m_Framebuffer;
        if (!fb)
            return 0;
        return fb->GetColorAttachmentRendererID(static_cast<u32>(index));
    }

    u32 GBuffer::GetDepthAttachmentID() const
    {
        const auto& fb = m_ResolvedFramebuffer ? m_ResolvedFramebuffer : m_Framebuffer;
        if (!fb)
            return 0;
        return fb->GetDepthAttachmentRendererID();
    }

    u32 GBuffer::GetMSColorAttachmentID(AttachmentIndex index) const
    {
        if (!m_Framebuffer)
            return 0;
        return m_Framebuffer->GetColorAttachmentRendererID(static_cast<u32>(index));
    }

    u32 GBuffer::GetMSDepthAttachmentID() const
    {
        if (!m_Framebuffer)
            return 0;
        return m_Framebuffer->GetDepthAttachmentRendererID();
    }

    void GBuffer::ResolveDepthOnly()
    {
        if (m_SampleCount <= 1 || !m_Framebuffer || !m_ResolvedFramebuffer)
            return;

        OLO_PROFILE_FUNCTION();

        const u32 srcFB = m_Framebuffer->GetRendererID();
        const u32 dstFB = m_ResolvedFramebuffer->GetRendererID();
        const GLint w = static_cast<GLint>(m_Width);
        const GLint h = static_cast<GLint>(m_Height);

        // Depth-only blit — skips colour resolves so per-sample colour data
        // stays intact for the MSAA deferred lighting shader to consume.
        glBlitNamedFramebuffer(srcFB, dstFB,
                               0, 0, w, h,
                               0, 0, w, h,
                               GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    }
} // namespace OloEngine
