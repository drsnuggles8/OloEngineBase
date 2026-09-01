#include "OloEnginePCH.h"
#include "OloEngine/Renderer/GBuffer.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/VertexArray.h"

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
            spec.Attachments.Attachments.reserve(GBuffer::s_ColorAttachmentFormats.size() + 1);
            for (const auto format : GBuffer::s_ColorAttachmentFormats)
                spec.Attachments.Attachments.emplace_back(format);

            // Depth must match the scene framebuffer's depth format
            // (`FramebufferTextureFormat::Depth` = DEPTH24STENCIL8) so that
            // `RenderCommand::BlitFramebuffer(RHI::BlitAspect::Depth, …)` — the path used
            // by `DeferredLightingPass` to hand G-Buffer depth to downstream
            // passes and by `SceneRenderPass::ResolveToScene` in forward+ —
            // succeeds. A format mismatch here surfaces as a per-frame flood
            // of `GL_INVALID_OPERATION: Depth formats do not match` and
            // leaves the scene-FB depth uninitialised, breaking every
            // downstream depth-read (overlays, foliage, decals, water,
            // SSAO/GTAO, fog, DoF, motion blur).
            spec.Attachments.Attachments.emplace_back(FramebufferTextureFormat::Depth);
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

        // Clamp to the device's reported MSAA capability. Drivers will silently
        // refuse to allocate multisample storage above GL_MAX_SAMPLES; on
        // capped hardware (4-sample) requesting 8 leaves the framebuffer
        // incomplete and every subsequent blit/lighting pass silently no-ops.
        if (sampleCount > 1)
        {
            const u32 maxSamples = std::max(RenderCommand::GetMaxFramebufferSamples(), 1u);
            const u32 deviceMax = static_cast<u32>(maxSamples);
            if (sampleCount > deviceMax)
            {
                OLO_CORE_WARN("GBuffer::Create: requested sample count {} exceeds device max {}; clamping.",
                              sampleCount, deviceMax);
                // Snap to the largest power-of-two from {1,2,4,8} that fits.
                if (deviceMax >= 8)
                    sampleCount = 8;
                else if (deviceMax >= 4)
                    sampleCount = 4;
                else if (deviceMax >= 2)
                    sampleCount = 2;
                else
                    sampleCount = 1;
            }
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

        const RHI::ResourceHandle srcFB = m_Framebuffer->GetRHIHandle();
        const RHI::ResourceHandle dstFB = m_ResolvedFramebuffer->GetRHIHandle();
        const i32 w = static_cast<i32>(m_Width);
        const i32 h = static_cast<i32>(m_Height);

        // Resolve each colour attachment independently — a framebuffer blit
        // only reads/writes the currently-selected read / draw attachment, so
        // this is the safe pattern for an MRT MSAA resolve.
        for (u32 i = 0; i < std::to_underlying(Count); ++i)
        {
            RenderCommand::SetFramebufferReadAttachment(srcFB, i);
            RenderCommand::SetFramebufferDrawAttachments(dstFB, std::array<u32, 1>{ i });
            RenderCommand::BlitFramebuffer(srcFB, dstFB,
                                           0, 0, w, h,
                                           0, 0, w, h,
                                           RHI::BlitAspect::Color, RHI::Filter::Nearest);
        }

        // Resolve depth (no sample filtering — GL_NEAREST is the only legal choice).
        RenderCommand::BlitFramebuffer(srcFB, dstFB,
                                       0, 0, w, h,
                                       0, 0, w, h,
                                       RHI::BlitAspect::Depth, RHI::Filter::Nearest);

        // RT2's alpha is a BITFIELD, and the blit above just averaged it
        // (issue #996). Put back the one channel averaging cannot express.
        ResolveFlagsLane();

        // Restore full draw-buffer set on the resolved FB so subsequent
        // passes that bind it for composition get all attachments.
        RenderCommand::RestoreAllFramebufferDrawAttachments(dstFB, std::to_underlying(Count));
    }

    void GBuffer::ResolveFlagsLane()
    {
        OLO_PROFILE_FUNCTION();

        if (m_SampleCount <= 1 || !m_Framebuffer || !m_ResolvedFramebuffer)
            return;

        // The A/B for this whole change: on, the lane is left exactly as the
        // average blit produced it, which is the pre-#996 behaviour and the
        // black fringe. Kept because the defect is invisible to every test that
        // does not read pixels, so the cheapest way to prove a frame difference
        // is attributable to this pass is to switch only this pass off.
        if (Levers::DisableGBufferFlagsResolve())
            return;

        // The three emissive channels are radiometry and keep the hardware's
        // averaging resolve; RT2's alpha carries the unlit bit and the PBR
        // closure model (oloEncodeGBufferPbrFlags, PBRCommon.glsl), and an
        // averaged bitfield decodes to a material nobody wrote — a ClosureV2
        // silhouette over Legacy averaged to exactly the UNLIT code and every
        // v2 object grew a raw-emissive black fringe. So the blit stays (that
        // is what keeps a Legacy-only frame byte-identical) and this pass
        // overwrites the alpha channel alone with the flags ONE REAL SAMPLE
        // wrote — an exactly-defined fetch on every backend and vendor. Which
        // sample wins (a lit one always outranks an unlit one) is documented in
        // GBufferFlagsResolve.glsl, beside the loop that decides it.
        if (!m_FlagsResolveShader && !m_FlagsResolveShaderFailed)
        {
            m_FlagsResolveShader = Shader::Create("assets/shaders/GBufferFlagsResolve.glsl");
            if (!m_FlagsResolveShader)
            {
                // Loud once, not once per frame: without this pass the flags
                // lane is the pre-#996 averaged bitfield, which is a shading
                // bug at every MSAA silhouette rather than a missing effect.
                m_FlagsResolveShaderFailed = true;
                OLO_CORE_ERROR("GBuffer::ResolveFlagsLane: failed to create assets/shaders/GBufferFlagsResolve.glsl - "
                               "the resolved-MSAA deferred path will mis-decode material flags at silhouettes.");
            }
        }
        if (!m_FlagsResolveShader)
            return;

        const RHI::ResourceHandle msEmissive = m_Framebuffer->GetColorAttachmentHandle(std::to_underlying(Emissive));
        if (!msEmissive.IsValid())
            return;

        const RHI::ResourceHandle dstFB = m_ResolvedFramebuffer->GetRHIHandle();

        m_ResolvedFramebuffer->Bind();
        RenderCommand::SetFramebufferDrawAttachments(dstFB, std::array<u32, 1>{ std::to_underlying(Emissive) });
        RenderCommand::SetViewport(0, 0, m_Width, m_Height);

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();

        // ALPHA ONLY. This is the whole point of the pass: the averaged
        // emissive RGB in the resolve target must survive untouched, so the
        // fragment shader's rgb output is masked out rather than computed.
        // SetColorMask is the GLOBAL setter, which is DEFINED as the indexed
        // one applied to every draw buffer (see
        // docs/agent-rules/gl-global-setter-resets-indexed-state.md), so it
        // both covers the single attachment selected above and is undone in
        // full by the all-true restore below — no per-attachment mask can
        // survive this pair.
        RenderCommand::SetColorMask(false, false, false, true);

        m_FlagsResolveShader->Bind();
        // Through the seam, not RenderCommand::BindTexture directly (ADR 0011
        // §5c / issue #691). GBufferFlagsResolve.glsl declares its input as a
        // slot-based `sampler2DMS`, so the seam takes its fallback and issues a
        // real bind — the same arrangement DeferredLighting_MSAA has, and for
        // the same reason: the descriptor heap has no reserved MULTISAMPLE
        // null, so there is no bindless form of this read to convert to. That
        // pairing is recorded in BindlessShaderPipelineTest's
        // kSlotBasedByDesign, beside the MSAA lighting variant's entry.
        HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_GBUFFER_EMISSIVE, msEmissive,
                                         RHI::HeapSlotLifetime::FrameTransient);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        HeapBinding::FlushOffsets();
        RenderCommand::DrawIndexed(va);

        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);

        // Back to the default framebuffer. Resolve() is called from the middle
        // of three different passes and there is no way to ask the RHI what was
        // bound before, so the choice is between a defined state every
        // downstream pass overrides by binding its own target, and leaving the
        // RESOLVE TARGET bound — which would quietly turn the next unguarded
        // draw into a G-Buffer write. Unbind is the safe half of that pair.
        m_ResolvedFramebuffer->Unbind();
    }

    u32 GBuffer::GetColorAttachmentID(AttachmentIndex index) const
    {
        const auto& fb = m_ResolvedFramebuffer ? m_ResolvedFramebuffer : m_Framebuffer;
        if (!fb)
            return 0;
        return fb->GetColorAttachmentRendererID(std::to_underlying(index));
    }

    u32 GBuffer::GetDepthAttachmentID() const
    {
        const auto& fb = m_ResolvedFramebuffer ? m_ResolvedFramebuffer : m_Framebuffer;
        if (!fb)
            return 0;
        return fb->GetDepthAttachmentRendererID();
    }

    RHI::ResourceHandle GBuffer::GetColorAttachmentHandle(AttachmentIndex index) const
    {
        const auto& fb = m_ResolvedFramebuffer ? m_ResolvedFramebuffer : m_Framebuffer;
        if (!fb)
            return RHI::NullResource;
        return fb->GetColorAttachmentHandle(std::to_underlying(index));
    }

    RHI::ResourceHandle GBuffer::GetDepthAttachmentHandle() const
    {
        const auto& fb = m_ResolvedFramebuffer ? m_ResolvedFramebuffer : m_Framebuffer;
        if (!fb)
            return RHI::NullResource;
        return fb->GetDepthAttachmentHandle();
    }

    u32 GBuffer::GetMSColorAttachmentID(AttachmentIndex index) const
    {
        if (!m_Framebuffer)
            return 0;
        return m_Framebuffer->GetColorAttachmentRendererID(std::to_underlying(index));
    }

    u32 GBuffer::GetMSDepthAttachmentID() const
    {
        if (!m_Framebuffer)
            return 0;
        return m_Framebuffer->GetDepthAttachmentRendererID();
    }

    RHI::ResourceHandle GBuffer::GetMSColorAttachmentHandle(AttachmentIndex index) const
    {
        if (!m_Framebuffer)
            return RHI::NullResource;
        return m_Framebuffer->GetColorAttachmentHandle(std::to_underlying(index));
    }

    RHI::ResourceHandle GBuffer::GetMSDepthAttachmentHandle() const
    {
        if (!m_Framebuffer)
            return RHI::NullResource;
        return m_Framebuffer->GetDepthAttachmentHandle();
    }

    void GBuffer::ResolveDepthOnly()
    {
        OLO_PROFILE_FUNCTION();

        if (m_SampleCount <= 1 || !m_Framebuffer || !m_ResolvedFramebuffer)
            return;

        const RHI::ResourceHandle srcFB = m_Framebuffer->GetRHIHandle();
        const RHI::ResourceHandle dstFB = m_ResolvedFramebuffer->GetRHIHandle();
        const i32 w = static_cast<i32>(m_Width);
        const i32 h = static_cast<i32>(m_Height);

        // Depth-only blit — skips colour resolves so per-sample colour data
        // stays intact for the MSAA deferred lighting shader to consume.
        RenderCommand::BlitFramebuffer(srcFB, dstFB,
                                       0, 0, w, h,
                                       0, 0, w, h,
                                       RHI::BlitAspect::Depth, RHI::Filter::Nearest);
    }
} // namespace OloEngine
