// =============================================================================
// FrameCaptureBackend.h
//
// Internal seam between the backend-neutral RenderGraphFrameCapture
// orchestration (Renderer/Debug/RenderGraphFrameCapture.cpp) and the GL-side
// operations the RendererAPI facade deliberately does not model — issue #691
// ADR 0011 §1.6 (what cannot convert in place RELOCATES).
//
// Same shape as StateGuardBackend.h: free functions declared here, DEFINED in
// Platform/OpenGL/OpenGLFrameCapture.cpp and resolved at link time, so the
// Renderer/Debug TU stays free of <glad/gl.h> and Platform/ includes. Nothing
// in this header names a GL type — the vocabulary is RHI handles, u32-opaque
// native values, and bools.
//
// Why these operations live behind a seam instead of the facade:
//   * the blit's save/neutralize/restore dance needs GL state QUERIES
//     (color writemask, scissor/sRGB/rasterizer-discard enables) and the
//     facade has no state-query family at all;
//   * GL_FRAMEBUFFER_SRGB / GL_RASTERIZER_DISCARD have no facade toggle;
//   * selecting the DEFAULT framebuffer's read buffer (GL_BACK) is not an
//     attachment index, so SetFramebufferReadAttachment cannot spell it;
//   * texture swizzle has no facade virtual;
//   * the facade exposes no error query, by design (ADR 0011 amendment (7)) —
//     the blit helper folds the glGetError attribution instead.
// =============================================================================

#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"

namespace OloEngine::Detail
{
    // Blit color from `srcFramebuffer` to `dstFramebuffer` over the full
    // (width x height) rect, nearest-filtered, with TRANSFER-OP SEMANTICS:
    // GL's blit honors GL_COLOR_WRITEMASK, GL_SCISSOR_TEST and
    // GL_FRAMEBUFFER_SRGB (and rasterizer discard), any of which silently
    // produces a black / partial / re-encoded capture — so the implementation
    // saves that state, neutralizes it, blits, and restores it. Vulkan's
    // vkCmdBlitImage behaves this way inherently.
    //
    // RHI::NullResource on either side names the DEFAULT framebuffer.
    //
    // Returns 0 on success, or the blit's native error code (opaque u32,
    // logged as hex by the caller) — the facade deliberately has no error
    // query (ADR 0011 amendment (7)), and a failed blit must not be recorded
    // as a capture.
    [[nodiscard]] u32 BlitWithGLTransferSemantics(RHI::ResourceHandle srcFramebuffer,
                                                  RHI::ResourceHandle dstFramebuffer,
                                                  u32 width, u32 height);

    // Opaque saved bindings for the default-framebuffer read-source selection
    // below. Native values, deliberately (u32-opaque): they never leave the
    // select/restore pair.
    struct DefaultFramebufferReadState
    {
        u32 ReadFramebuffer = 0;
        u32 DrawFramebuffer = 0;
        u32 ReadBuffer = 0;
    };

    // Save the read/draw framebuffer bindings and the current read-buffer
    // selection, then point the DEFAULT framebuffer's read buffer at the
    // backbuffer so a subsequent BlitWithGLTransferSemantics(NullResource, ...)
    // reads the presented image.
    [[nodiscard]] DefaultFramebufferReadState SelectDefaultFramebufferReadSource();

    // Undo SelectDefaultFramebufferReadSource exactly: rebind the saved read
    // framebuffer, restore ITS read-buffer selection, rebind the saved draw
    // framebuffer.
    void RestoreDefaultFramebufferReadSource(const DefaultFramebufferReadState& state);

    // Force alpha = 1 on sample for a capture texture (GL texture swizzle,
    // which the facade does not model). See the caller for why: scene
    // framebuffers store alpha = 0 and ImGui draws the thumbnails blended.
    void SetTextureAlphaSwizzleOne(RHI::ResourceHandle texture);
} // namespace OloEngine::Detail
