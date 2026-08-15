#pragma once

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#include <glad/gl.h>

namespace OloEngine::Utils
{
    // ---------------------------------------------------------------------
    // Handle -> GL name. The backend's half of issue #691 Phase 2 step 3.
    //
    // One of the two sanctioned ways out of an RHI::ResourceHandle (the other
    // is GetNativeHandleForDebug, for Renderer/Debug/). It lives HERE, inside
    // Platform/OpenGL/, because this is the only place a GL name may be named
    // at all — RHIBoundaryRatchetTest's backend_resolve_hatch counter baselines
    // uses of the underlying ResolveNativeForBackend outside Platform/ at zero,
    // and putting a resolving helper in Renderer/ to save typing would breach
    // exactly the boundary this phase exists to close.
    //
    // A stale or null handle yields 0, which is benign in every GL call the
    // backend makes: binding 0 unbinds, and glDelete*(0) is a defined no-op. So
    // a use-after-free degrades to "nothing bound" — visibly wrong rather than
    // silently sampling whatever object inherited the recycled name, which is
    // the failure the bare u32 could not distinguish.
    // ---------------------------------------------------------------------
    [[nodiscard]] inline GLuint ResolveNative(RHI::ResourceHandle handle) noexcept
    {
        return static_cast<GLuint>(RHI::ResourceRegistry::Get().ResolveNativeForBackend(handle));
    }

    // Kind-checked form. GL names are per-object-type, so a buffer and a texture
    // can both legitimately be name 1 — meaning a handle passed to the wrong
    // family resolves to a real, valid, completely unrelated GL object rather
    // than failing. The generation cannot catch that: both handles are live.
    //
    // Prefer this wherever the call site knows the family it wants (every
    // texture bind, every buffer bind). Returns 0 on a mismatch, which lands on
    // the same benign degradation the unchecked form documents above: binding 0
    // unbinds, glDelete*(0) is a no-op.
    // True only for a LIVE handle of the wrong family. A stale or null handle is
    // deliberately NOT a mismatch: KindOf reports Unknown for it, and a stale
    // handle is the documented benign path above (resolves to 0, binding 0
    // unbinds) — treating it as a mismatch would log on every legitimate
    // use-after-free degradation and bury the real signal.
    [[nodiscard]] inline bool IsWrongKind(RHI::ResourceHandle handle, RHI::ResourceKind expected) noexcept
    {
        const RHI::ResourceKind actual = RHI::ResourceRegistry::Get().KindOf(handle);
        return actual != RHI::ResourceKind::Unknown && actual != expected;
    }

    [[nodiscard]] inline GLuint ResolveNativeAs(RHI::ResourceHandle handle, RHI::ResourceKind expected) noexcept
    {
        if (IsWrongKind(handle, expected))
        {
            OLO_CORE_WARN("ResolveNativeAs: handle {} is a {}, not a {} — refusing to resolve it as one.",
                          handle, RHI::ToString(RHI::ResourceRegistry::Get().KindOf(handle)),
                          RHI::ToString(expected));
            return 0u;
        }
        return static_cast<GLuint>(RHI::ResourceRegistry::Get().ResolveNativeForBackend(handle));
    }
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

    // THE TWO CALLS EVERY TEXTURE DESTRUCTOR OWES, in one place so a new texture
    // type cannot ship with only one of them (issue #691 Phase 3).
    //
    //   * `CommandDispatch::InvalidateTextureBinding` drops the slot path's
    //     "this unit already has this texture" cache, so a future bind with a
    //     recycled GL name is not skipped against stale tracking.
    //   * `DescriptorHeap::RetireResource` drops the HEAP's descriptors, which
    //     name the underlying GL OBJECT and so dangle the moment it is deleted.
    //     Sampling a dead bindless handle is undefined behaviour, not a black
    //     read (ADR 0011 amendment (22)).
    //
    // Both were already paid by `~OpenGLTexture2D` and `~OpenGLTextureCubemap`
    // and by neither `~OpenGLTexture2DArray` nor `~OpenGLTexture3D` — which is
    // exactly the drift a per-site rule produces, and it was live: both of those
    // types mint an RHI handle and are bound as storage-image descriptors
    // through the heap (the wind field, the fog scatter volumes, the cloud noise
    // volumes, the ocean FFT ping-pong array). Destroying one left a resident
    // image handle on a deleted texture, logged at shutdown as a single
    // `GL_INVALID_OPERATION: Not a valid texture`.
    //
    // `noexcept` IS THE POINT, not decoration. A destructor is implicitly
    // noexcept, so anything thrown out of it calls std::terminate;
    // `RetireResource` takes the heap's mutex and touches containers, either of
    // which can throw. Leaking a descriptor is recoverable, taking the process
    // down during teardown is not — so this swallows and logs, exactly as
    // `~OpenGLFramebuffer` already does by hand for its attachments.
    void RetireTextureViews(RHI::ResourceHandle handle) noexcept;

    [[nodiscard("Store this!")]] constexpr GLenum TextureTarget(const bool multisampled) noexcept;
    void PrepareTexture(const u32 id, const int samples, const GLenum format, const int width, const int height);
    void CreateTextures(const bool multisampled, const int count, u32* const outID);
    void BindTextures(const u32 firstID, const u32 count, const GLuint* id);
    void AttachColorTexture(const u32 fbo, const u32 id, const int samples, const GLenum internalFormat, const int width, const int height, const u32 index);
    void AttachDepthTexture(const u32 fbo, const u32 id, const int samples, const GLenum format, const GLenum attachmentType, const int width, const int height);
    [[nodiscard("Store this!")]] bool IsDepthFormat(const FramebufferTextureFormat format) noexcept;
    [[nodiscard("Store this!")]] GLenum OloFBTextureFormatToGL(const FramebufferTextureFormat format);
    [[nodiscard("Store this!")]] GLenum OloFBColorTextureFormatToGL(const FramebufferTextureFormat format);
    [[nodiscard("Store this!")]] GLenum OloFBDepthTextureFormatToGL(const FramebufferTextureFormat format);
} // namespace OloEngine::Utils
