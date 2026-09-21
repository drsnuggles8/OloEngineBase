#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"

#include <array>

namespace OloEngine
{
    // @brief 6-RT G-Buffer for the deferred renderer.
    //
    // Layout (matches the plan in /memories/session/plan.md and the slot
    // constants in ShaderBindingLayout::TEX_GBUFFER_*):
    //
    //   RT0 (RGBA8)       — Albedo RGB + Metallic A
    //   RT1 (RGBA16F)     — Octahedral-encoded world-space normal (xy) +
    //                       Roughness (z) + AO (w)
    //                       (GTAO converts to view-space at runtime via u_ViewMatrix)
    //   RT2 (RGBA16F)     — Emissive RGB + packed material-flags A
    //   RT3 (RG16F)       — Screen-space velocity (previous→current)
    //   RT4 (R32I)        — Picking entity ID (cleared to -1 each frame).
    //                       Blitted into SceneColor RT1 by DeferredLightingPass
    //                       so the SelectionOutline JFA Init sees per-pixel
    //                       entity IDs in Deferred just like Forward.
    //   RT5 (RGBA16F)     — Baked lightmap irradiance E (rgb) + coverage (a),
    //                       issue #865. The G-Buffer pass does the atlas fetch
    //                       (it is the only stage that still has UV2 and the
    //                       per-draw atlas region); the lighting pass consumes
    //                       the result as the ambient ladder's top rung, exactly
    //                       as the forward path consumes
    //                       sampleLightmapIrradiance()'s vec4. Storing the
    //                       IRRADIANCE rather than the atlas UV is deliberate:
    //                       an MSAA resolve averages colour attachments, and
    //                       averaging two charts' UVs across a silhouette reads
    //                       an unrelated atlas texel, whereas averaging E and
    //                       coverage together is exactly the alpha-weighted
    //                       blend the sampler already documents. Every G-Buffer
    //                       writer must write this target (vec4(0) = "no baked
    //                       GI here"); an unwritten MRT output is undefined, and
    //                       an undefined .a reads as coverage.
    //   Depth (D32F)      — shared with subsequent lighting / OIT passes
    //
    // The class is a thin convenience wrapper around a Framebuffer. The
    // G-Buffer write (SceneRenderPass) and the read (DeferredLightingPass)
    // drive actual usage; an MSAA variant came later.
    class GBuffer : public RefCounted
    {
      public:
        enum AttachmentIndex : u32
        {
            Albedo = 0,   // RGBA8       — base colour + metallic
            Normal = 1,   // RGBA16F     — octahedral normal + roughness + AO
            Emissive = 2, // RGBA16F     — emissive + material flags
            Velocity = 3, // RG16F       — screen-space velocity
            EntityID = 4, // RED_INTEGER — per-pixel picking entity ID (cleared to -1)
            BakedGI = 5,  // RGBA16F     — baked lightmap irradiance + coverage (issue #865)
            Count = 6
        };

        // The production-owned colour-attachment contract in AttachmentIndex
        // order. GBuffer.cpp builds both the draw and resolve framebuffers from
        // this table, and reflection tests compare every full G-Buffer writer
        // against it, so shader numeric types and attachment formats cannot
        // drift as independent mirrors.
        inline static constexpr std::array<FramebufferTextureFormat, Count> s_ColorAttachmentFormats = {
            FramebufferTextureFormat::RGBA8,
            FramebufferTextureFormat::RGBA16F,
            FramebufferTextureFormat::RGBA16F,
            FramebufferTextureFormat::RG16F,
            FramebufferTextureFormat::RED_INTEGER,
            FramebufferTextureFormat::RGBA16F,
        };

        // Create a G-Buffer sized to (width, height). sampleCount = 1 means
        // no MSAA (2/4/8 are allowed elsewhere). width/height must be > 0 or
        // creation is deferred (call Resize() to populate).
        [[nodiscard]] static Ref<GBuffer> Create(u32 width, u32 height, u32 sampleCount = 1);

        ~GBuffer() = default;

        // Resize the backing framebuffer. Triggers attachment reallocation.
        void Resize(u32 width, u32 height);

        // Current dimensions.
        [[nodiscard]] u32 GetWidth() const noexcept
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const noexcept
        {
            return m_Height;
        }
        [[nodiscard]] u32 GetSampleCount() const noexcept
        {
            return m_SampleCount;
        }

        // Underlying framebuffer for passes that need MRT bind/clear.
        // In MSAA mode this is the multisample G-Buffer that geometry
        // writes into; sample textures come from GetSamplingFramebuffer.
        [[nodiscard]] const Ref<Framebuffer>& GetFramebuffer() const noexcept
        {
            return m_Framebuffer;
        }

        // Framebuffer whose texture attachments should be bound as samplers
        // by DeferredLightingPass / OITResolvePass. For sampleCount == 1
        // this is identical to GetFramebuffer(). For sampleCount > 1 this
        // is the single-sample resolve target populated by Resolve().
        [[nodiscard]] const Ref<Framebuffer>& GetSamplingFramebuffer() const noexcept
        {
            return m_ResolvedFramebuffer ? m_ResolvedFramebuffer : m_Framebuffer;
        }

        // MSAA resolve: blits each colour attachment + depth from the
        // multisample G-Buffer to the single-sample resolve target. No-op
        // when sampleCount == 1. Must be called after all MRT writes
        // (scene geometry + deferred-path decals) and before DeferredLightingPass.
        void Resolve();

        // MSAA depth-only resolve — populates only the depth attachment of
        // the resolve framebuffer, leaving the colour attachments stale.
        // Used by the per-sample deferred lighting path: decals
        // sample the resolved depth (single-sample) to reconstruct world
        // position, while writing into the multisample G-Buffer; the
        // lighting shader then samples the still-multisample colour
        // attachments per-sample. No-op when sampleCount == 1.
        void ResolveDepthOnly();

        // Renderer IDs per attachment — handy for binding as samplers in
        // DeferredLightingPass / OITResolvePass. These
        // return resolved (single-sample) IDs when MSAA is active.
        [[nodiscard]] u32 GetColorAttachmentID(AttachmentIndex index) const;
        [[nodiscard]] u32 GetDepthAttachmentID() const;
        // Identity forms (issue #691). The G-Buffer attachments are
        // ordinary framebuffer attachments, so these just forward to the
        // framebuffer's own handle accessors.
        [[nodiscard]] RHI::ResourceHandle GetColorAttachmentHandle(AttachmentIndex index) const;
        [[nodiscard]] RHI::ResourceHandle GetDepthAttachmentHandle() const;

        // Raw multisample attachment IDs. For sampleCount == 1 these are
        // identical to GetColorAttachmentID / GetDepthAttachmentID. For
        // sampleCount > 1 these bind as `sampler2DMS` / `sampler2DMSArray`
        // in the per-sample deferred lighting shader.
        [[nodiscard]] u32 GetMSColorAttachmentID(AttachmentIndex index) const;
        [[nodiscard]] u32 GetMSDepthAttachmentID() const;
        // Identity forms of the two above (issue #691). Needed
        // because the per-sample MSAA paths bind and COPY the multisample
        // attachments, and the facade takes identities only.
        [[nodiscard]] RHI::ResourceHandle GetMSColorAttachmentHandle(AttachmentIndex index) const;
        [[nodiscard]] RHI::ResourceHandle GetMSDepthAttachmentHandle() const;

      private:
        GBuffer(u32 width, u32 height, u32 sampleCount);
        void Recreate();

        // Overwrite the resolve target's RT2 ALPHA with the flags ONE sample of
        // the multisample RT2 wrote, undoing the average blit for the one channel
        // that is a bitfield rather than radiometry (issue #996). Runs inside
        // Resolve() so no caller can forget it; a no-op when sampleCount == 1.
        void ResolveFlagsLane();

        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_SampleCount = 1;
        Ref<Framebuffer> m_Framebuffer;
        Ref<Framebuffer> m_ResolvedFramebuffer; // null when m_SampleCount == 1

        // GBufferFlagsResolve.glsl — the alpha-only fixup Resolve() runs after
        // the average blit (issue #996). Created lazily on the first MSAA
        // resolve and owned by this GBuffer, so a single-sample G-Buffer (and
        // every headless session that never resolves) compiles nothing.
        Ref<Shader> m_FlagsResolveShader;
        bool m_FlagsResolveShaderFailed = false;
    };
} // namespace OloEngine
