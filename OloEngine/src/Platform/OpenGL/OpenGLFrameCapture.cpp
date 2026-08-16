// =============================================================================
// OpenGLFrameCapture.cpp
//
// GL definitions for the RenderGraphFrameCapture seam declared in
// Renderer/Debug/FrameCaptureBackend.h (#691 Phase 9, ADR 0011 §1.6). The
// orchestration — capture cache, hook lifecycle, probe bookkeeping — stays in
// Renderer/Debug/RenderGraphFrameCapture.cpp; everything that needs glad or a
// GL state query lives here.
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/FrameCaptureBackend.h"

#include "Platform/OpenGL/OpenGLUtilities.h"

#include <glad/gl.h>

namespace OloEngine::Detail
{
    u32 BlitWithGLTransferSemantics(RHI::ResourceHandle srcFramebuffer,
                                    RHI::ResourceHandle dstFramebuffer,
                                    u32 width, u32 height)
    {
        // RHI::NullResource resolves to 0 = the DEFAULT framebuffer.
        const GLuint srcID = Utils::ResolveNativeAs(srcFramebuffer, RHI::ResourceKind::Framebuffer);
        const GLuint dstID = Utils::ResolveNativeAs(dstFramebuffer, RHI::ResourceKind::Framebuffer);

        // glBlitFramebuffer honors GL_COLOR_WRITEMASK, GL_SCISSOR_TEST, and
        // GL_FRAMEBUFFER_SRGB. Various passes leave color writes disabled
        // (depth prepass), scissor enabled to a sub-region (shadow tiles),
        // or sRGB-encode on, all of which would silently produce a black /
        // partial capture. Save current state, neutralize, blit, restore.
        GLboolean prevColorMask[4]{};
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
        const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
        const GLboolean prevSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        const GLboolean prevRasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (prevScissor)
        {
            glDisable(GL_SCISSOR_TEST);
        }
        if (prevSrgb)
        {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
        if (prevRasterizerDiscard)
        {
            glDisable(GL_RASTERIZER_DISCARD);
        }

        // Drain first: GL holds an error until someone reads it, so a stale
        // error from unrelated earlier work would be attributed to this blit
        // and suppress a capture that actually succeeded (review finding).
        Utils::DrainGLErrors();
        glBlitNamedFramebuffer(srcID, dstID,
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Folded error attribution: the facade exposes no error query (ADR
        // 0011 amendment (7)), but a failed blit must not be recorded as a
        // capture, so the code crosses the seam as an opaque u32.
        const GLenum blitErr = glGetError();

        // Restore prior global state so we don't perturb the next pass.
        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
        if (prevScissor)
        {
            glEnable(GL_SCISSOR_TEST);
        }
        if (prevSrgb)
        {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
        if (prevRasterizerDiscard)
        {
            glEnable(GL_RASTERIZER_DISCARD);
        }

        return static_cast<u32>(blitErr);
    }

    DefaultFramebufferReadState SelectDefaultFramebufferReadSource()
    {
        DefaultFramebufferReadState state;

        GLint prevReadFramebuffer = 0;
        GLint prevDrawFramebuffer = 0;
        GLint prevReadBuffer = GL_BACK;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFramebuffer);
        glGetIntegerv(GL_READ_BUFFER, &prevReadBuffer);
        state.ReadFramebuffer = static_cast<u32>(prevReadFramebuffer);
        state.DrawFramebuffer = static_cast<u32>(prevDrawFramebuffer);
        state.ReadBuffer = static_cast<u32>(prevReadBuffer);

        // glReadBuffer acts on the CURRENTLY BOUND read framebuffer, so the
        // default framebuffer must be bound before its read buffer can be
        // pointed at the backbuffer.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);

        return state;
    }

    void RestoreDefaultFramebufferReadSource(const DefaultFramebufferReadState& state)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(state.ReadFramebuffer));
        glReadBuffer(static_cast<GLenum>(state.ReadBuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(state.DrawFramebuffer));
    }

    void SetTextureAlphaSwizzleOne(RHI::ResourceHandle texture)
    {
        const GLuint textureID = Utils::ResolveNativeAs(texture, RHI::ResourceKind::Texture);
        glTextureParameteri(textureID, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
} // namespace OloEngine::Detail
