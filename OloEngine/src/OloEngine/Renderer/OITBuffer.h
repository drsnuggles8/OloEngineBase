#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"

namespace OloEngine
{
    // @brief Two-attachment framebuffer for weighted-blended OIT (McGuire & Bavoil 2013).
    //
    // Layout (McGuire & Bavoil 2013):
    //   RT0 (RGBA16F) — Accum: sum(Ci * ai * wi) in RGB, sum(ai * wi) in A
    //   RT1 (RG16F)   — Revealage: prod(1 - ai) for final alpha reveal
    //   Depth         — Read-only scene depth (blit-copied from scene FB)
    //
    // Transparent passes render with per-attachment blend funcs:
    //   RT0: GL_ONE, GL_ONE              (additive)
    //   RT1: GL_ZERO, GL_ONE_MINUS_SRC_COLOR (multiplicative, stores revealage in R)
    //
    // OITResolveRenderPass composites accum/revealage over the scene FB in a
    // single fullscreen draw.
    class OITBuffer : public RefCounted
    {
      public:
        enum AttachmentIndex : u32
        {
            Accum = 0,
            Revealage = 1,
            Count = 2
        };

        // width/height must be > 0 or creation falls back to 1x1 placeholder.
        [[nodiscard]] static Ref<OITBuffer> Create(u32 width, u32 height);

        ~OITBuffer() = default;

        void Resize(u32 width, u32 height);

        [[nodiscard]] u32 GetWidth() const noexcept
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const noexcept
        {
            return m_Height;
        }

        // Underlying framebuffer for MRT bind during transparent passes.
        [[nodiscard]] const Ref<Framebuffer>& GetFramebuffer() const noexcept
        {
            return m_Framebuffer;
        }

        // Texture IDs for the resolve shader.
        [[nodiscard]] u32 GetAccumAttachmentID() const;
        [[nodiscard]] u32 GetRevealageAttachmentID() const;

        // Clears accum to (0,0,0,0) and revealage to 1.0 (fully-revealed background).
        // When a source framebuffer is supplied, its depth attachment is
        // blit-copied into the OIT depth attachment so WB-OIT fragments are
        // correctly occluded by opaque geometry (water/decals behind walls
        // don't bleed through). With no source or a format-incompatible
        // source, depth is cleared to the far plane and transparent fragments
        // render on top of everything — same behaviour as before the depth
        // blit was introduced.
        //
        // Frame-token variant: the clear only runs when `frameToken !=
        // m_LastClearedFrameToken`, making the clear ordering an explicit
        // caller-owned dependency rather than hidden per-frame state. Pass
        // the same token from every transparent pass in a frame (water,
        // decals, particles) so the first call clears and subsequent calls
        // no-op. OITResolvePass does NOT need to reset any flag; the next
        // frame's higher token value auto-re-arms the clear.
        void ClearForFrame(u64 frameToken, const Ref<Framebuffer>& sourceDepth = nullptr);

        // Legacy bool-guarded overload. Uses an internal monotonic counter
        // so existing callers that don't plumb a frame token still get the
        // "idempotent per frame" behaviour. Prefer the token variant for
        // new code. `ResetClearFlag()` re-arms this counter path.
        void ClearForFrame(const Ref<Framebuffer>& sourceDepth = nullptr);

        // Re-arms the legacy bool-guarded path (no-op for the token variant).
        // OITResolvePass calls this after compositing so the next frame's
        // first transparent pass clears.
        void ResetClearFlag() noexcept
        {
            m_LegacyClearedThisFrame = false;
        }

      private:
        OITBuffer(u32 width, u32 height);
        void Recreate();

        u32 m_Width = 0;
        u32 m_Height = 0;
        Ref<Framebuffer> m_Framebuffer;
        // Token of the most recent frame this buffer was cleared for. 0 is
        // reserved as "never cleared" so a caller passing frameToken==0 will
        // always clear. Flipped by the token-based ClearForFrame() only.
        u64 m_LastClearedFrameToken = 0;
        // Legacy flag path: true if the bool-guarded ClearForFrame() overload
        // ran this frame. ResetClearFlag() resets it.
        bool m_LegacyClearedThisFrame = false;
    };
} // namespace OloEngine
