#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"

namespace OloEngine
{
    // @brief 4-RT G-Buffer for the deferred renderer.
    //
    // Layout (matches the plan in /memories/session/plan.md and the slot
    // constants in ShaderBindingLayout::TEX_GBUFFER_*):
    //
    //   RT0 (RGBA8)       — Albedo RGB + Metallic A
    //   RT1 (RGBA16F)     — Octahedral-encoded view-space normal (xy) +
    //                       Roughness (z) + AO (w)
    //   RT2 (RGBA16F)     — Emissive RGB + packed material-flags A
    //   RT3 (RG16F)       — Screen-space velocity (previous→current)
    //   Depth (D32F)      — shared with subsequent lighting / OIT passes
    //
    // The class is a thin convenience wrapper around a Framebuffer. Phase 2
    // (SceneRenderPass G-Buffer write) and Phase 3 (DeferredLightingPass)
    // drive actual usage; Phase 5 introduces an MSAA variant.
    class GBuffer : public RefCounted
    {
      public:
        enum AttachmentIndex : u32
        {
            Albedo = 0,    // RGBA8   — base colour + metallic
            Normal = 1,    // RGBA16F — octahedral normal + roughness + AO
            Emissive = 2,  // RGBA16F — emissive + material flags
            Velocity = 3,  // RG16F   — screen-space velocity
            Count = 4
        };

        // Create a G-Buffer sized to (width, height). sampleCount = 1 means
        // no MSAA (Phase 5 will allow 2/4/8). width/height must be > 0 or
        // creation is deferred (call Resize() to populate).
        [[nodiscard]] static Ref<GBuffer> Create(u32 width, u32 height, u32 sampleCount = 1);

        ~GBuffer() = default;

        // Resize the backing framebuffer. Triggers attachment reallocation.
        void Resize(u32 width, u32 height);

        // Current dimensions.
        [[nodiscard]] u32 GetWidth() const noexcept { return m_Width; }
        [[nodiscard]] u32 GetHeight() const noexcept { return m_Height; }
        [[nodiscard]] u32 GetSampleCount() const noexcept { return m_SampleCount; }

        // Underlying framebuffer for passes that need MRT bind/clear.
        // In MSAA mode this is the multisample G-Buffer that geometry
        // writes into; sample textures come from GetSamplingFramebuffer.
        [[nodiscard]] const Ref<Framebuffer>& GetFramebuffer() const noexcept { return m_Framebuffer; }

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
        // Used by the per-sample deferred lighting path (Phase 5): decals
        // sample the resolved depth (single-sample) to reconstruct world
        // position, while writing into the multisample G-Buffer; the
        // lighting shader then samples the still-multisample colour
        // attachments per-sample. No-op when sampleCount == 1.
        void ResolveDepthOnly();

        // Renderer IDs per attachment — handy for binding as samplers in
        // DeferredLightingPass / OITResolvePass (Phase 3 / Phase 6). These
        // return resolved (single-sample) IDs when MSAA is active.
        [[nodiscard]] u32 GetColorAttachmentID(AttachmentIndex index) const;
        [[nodiscard]] u32 GetDepthAttachmentID() const;

        // Raw multisample attachment IDs. For sampleCount == 1 these are
        // identical to GetColorAttachmentID / GetDepthAttachmentID. For
        // sampleCount > 1 these bind as `sampler2DMS` / `sampler2DMSArray`
        // in the per-sample deferred lighting shader.
        [[nodiscard]] u32 GetMSColorAttachmentID(AttachmentIndex index) const;
        [[nodiscard]] u32 GetMSDepthAttachmentID() const;

      private:
        GBuffer(u32 width, u32 height, u32 sampleCount);
        void Recreate();

        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_SampleCount = 1;
        Ref<Framebuffer> m_Framebuffer;
        Ref<Framebuffer> m_ResolvedFramebuffer; // null when m_SampleCount == 1
    };
} // namespace OloEngine
