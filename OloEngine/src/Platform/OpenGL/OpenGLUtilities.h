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
            // glIsProgram guards against a program deleted between construction
            // and destruction (issue #625): a program deleted while still bound
            // elsewhere is only deletion-*flagged* by GL until something unbinds
            // it — which this guard's own constructor may just have done. Should
            // that be the completing unbind, m_PreviousProgram is no longer a
            // valid id and restoring it would raise GL_INVALID_VALUE. Skipping
            // the restore leaves 0 bound, which is safe: the next real draw binds
            // its own program explicitly.
            if (m_PreviousProgram != 0 && glIsProgram(static_cast<GLuint>(m_PreviousProgram)))
                glUseProgram(static_cast<GLuint>(m_PreviousProgram));
        }
        GLClearProgramGuard(const GLClearProgramGuard&) = delete;
        GLClearProgramGuard& operator=(const GLClearProgramGuard&) = delete;
        GLClearProgramGuard(GLClearProgramGuard&&) = delete;
        GLClearProgramGuard& operator=(GLClearProgramGuard&&) = delete;

      private:
        GLint m_PreviousProgram = 0;
    };

    // Unbinds `program` from GL_CURRENT_PROGRAM if it is currently bound. Call
    // this immediately before glDeleteProgram(program) at any site where the
    // program may still be bound — deferred deletions in particular (issue
    // #625): by the time a FrameResourceManager-deferred deletion lambda
    // actually runs, an unrelated render tick may have left this exact program
    // bound with nothing since to unbind it. Deleting a still-bound program
    // only *flags* it for deletion — GL keeps it valid and current until
    // something else calls glUseProgram, which then completes the deletion.
    // Left unhandled, that completing unbind is commonly GLClearProgramGuard's
    // own scoped unbind, which then fails to restore the now-invalid id
    // (GL_INVALID_VALUE). Unbinding here first makes the deletion immediate and
    // deterministic instead of a landmine for later code.
    inline void UnbindProgramIfCurrent(const u32 program) noexcept
    {
        if (program == 0)
            return;
        GLint current = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &current);
        if (static_cast<u32>(current) == program)
            glUseProgram(0);
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
