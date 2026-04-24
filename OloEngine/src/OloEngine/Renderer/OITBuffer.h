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
    //   RT1 (R16F)    — Revealage: prod(1 - ai) for final alpha reveal
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
        // Idempotent per frame: the first call clears; subsequent calls no-op
        // until `ResetClearFlag()` is invoked (typically by OITResolvePass at
        // the end of its Execute). This allows multiple transparent passes
        // (water, decals, particles) to safely call ClearForFrame() before
        // accumulating, without wiping each other's contributions.
        void ClearForFrame(const Ref<Framebuffer>& sourceDepth = nullptr);

        // Called by OITResolvePass after compositing to re-arm the clear for
        // the next frame's first transparent pass. Does NOT clear the buffer.
        void ResetClearFlag() noexcept
        {
            m_ClearedThisFrame = false;
        }

      private:
        OITBuffer(u32 width, u32 height);
        void Recreate();

        u32 m_Width = 0;
        u32 m_Height = 0;
        Ref<Framebuffer> m_Framebuffer;
        bool m_ClearedThisFrame = false;
    };
} // namespace OloEngine
