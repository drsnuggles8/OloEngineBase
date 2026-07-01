#pragma once

#include "OloEngine/Renderer/Framebuffer.h"
#include <glad/gl.h>

namespace OloEngine::Utils
{
    // Drain any pending GL error(s) so a subsequent glGetError() check reflects
    // only the operation it guards, not an error leaked in by an unrelated earlier
    // GL call in the same context. Without this, a leaked error is misattributed
    // to the next checked call (e.g. a texture readback wrongly reports failure —
    // the ProceduralSkyBakeTest cross-suite flake). The 64-iteration bound keeps a
    // lost context (glGetError stuck on GL_CONTEXT_LOST) from spinning forever.
    inline void DrainGLErrors() noexcept
    {
        // Hoist the iteration cap out of the condition. GL only ever queues a
        // handful of distinct error flags, so this is unreachable in normal
        // operation; it exists only so a lost context (glGetError stuck on
        // GL_CONTEXT_LOST) cannot spin forever.
        constexpr int kMaxDrainIterations = 64;
        for (int guard = 0; guard < kMaxDrainIterations && glGetError() != GL_NO_ERROR; ++guard)
        {
        }
    }

    [[nodiscard("Store this!")]] constexpr GLenum TextureTarget(const bool multisampled) noexcept;
    void PrepareTexture(const u32 id, const int samples, const GLenum format, const int width, const int height);
    void CreateTextures(const bool multisampled, const int count, u32* const outID);
    void BindTexture(const u32 id);
    void BindTextures(const u32 firstID, const u32 count, const GLuint* id);
    void AttachColorTexture(const u32 fbo, const u32 id, const int samples, const GLenum internalFormat, const int width, const int height, const u32 index);
    void AttachDepthTexture(const u32 fbo, const u32 id, const int samples, const GLenum format, const GLenum attachmentType, const int width, const int height);
    [[nodiscard("Store this!")]] bool IsDepthFormat(const FramebufferTextureFormat format) noexcept;
    [[nodiscard("Store this!")]] GLenum OloFBTextureFormatToGL(const FramebufferTextureFormat format);
    [[nodiscard("Store this!")]] GLenum OloFBColorTextureFormatToGL(const FramebufferTextureFormat format);
    [[nodiscard("Store this!")]] GLenum OloFBDepthTextureFormatToGL(const FramebufferTextureFormat format);
} // namespace OloEngine::Utils
