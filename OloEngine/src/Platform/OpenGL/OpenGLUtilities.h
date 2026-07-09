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

    // Scoped program unbind around glClear / glClearBuffer* calls. NVIDIA
    // revalidates the *currently bound* program against the currently bound
    // framebuffer during a clear and JIT-recompiles its vertex shader when the
    // state differs from what it was last validated against (debug id 131218)
    // — a pure waste when the program left bound by the previous pass will
    // never draw into this framebuffer. Unbinding for just the clear avoids
    // that; restoring afterwards keeps the bind state every caller (and the
    // CommandDispatch shader-bind cache) believes in. See
    // docs/agent-rules/gl-clear-program-revalidation.md.
    class GLClearProgramGuard
    {
      public:
        GLClearProgramGuard() noexcept
        {
            glGetIntegerv(GL_CURRENT_PROGRAM, &m_PreviousProgram);
            if (m_PreviousProgram != 0)
                glUseProgram(0);
        }
        ~GLClearProgramGuard()
        {
            if (m_PreviousProgram != 0)
                glUseProgram(static_cast<GLuint>(m_PreviousProgram));
        }
        GLClearProgramGuard(const GLClearProgramGuard&) = delete;
        GLClearProgramGuard& operator=(const GLClearProgramGuard&) = delete;
        GLClearProgramGuard(GLClearProgramGuard&&) = delete;
        GLClearProgramGuard& operator=(GLClearProgramGuard&&) = delete;

      private:
        GLint m_PreviousProgram = 0;
    };

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
