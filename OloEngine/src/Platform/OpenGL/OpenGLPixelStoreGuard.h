#pragma once

// =============================================================================
// OpenGLPixelStoreGuard.h — tightly-packed readbacks, restored afterwards
//
// GL's default `GL_PACK_ALIGNMENT` is **4**: each row a readback writes is
// padded up to a 4-byte boundary. Every `GetData`-style path in this backend
// sizes its destination buffer TIGHTLY (`width * height * bytesPerPixel`) and
// hands that size to `glGetTextureImage`, which validates it against the
// padded requirement — so any row whose byte count is not a multiple of 4 makes
// the call fail with `GL_INVALID_OPERATION` and the readback returns nothing.
//
// In practice that is every RGB8 texture whose width is not a multiple of 4
// (a 3-wide RGB8 row is 9 bytes, padded to 12), and it is silent: the caller
// sees "readback failed", not "your alignment is wrong".
//
// Measured, not theorised: `ReferenceTextureCaptureGpu.
// AnRgb8TextureWhoseRowIsNotFourByteAlignedStillReadsBack` fails with GL error
// 1282 without this guard and passes with it. It was found because the #869
// bake reads material albedo back to the CPU, where such a texture silently
// baked factor-only.
//
// The SAME defect exists in the other direction, on the same formats, and is
// fixed here too. `GL_UNPACK_ALIGNMENT` also defaults to 4, so uploading a
// tightly-packed 3-wide RGB8 image makes GL read each row with a 12-byte stride
// out of a 9-byte row — no error, just shifted pixels. The cubemap loader
// already set unpack alignment for its own uploads; `OpenGLTexture2D::SetData`
// did not, which is how the regression test above first came back with the
// right *shape* and the wrong *values*.
//
// Both scopes save and restore rather than setting globally: pixel-store state
// is shared, and neither an upload nor a readback has any business changing it
// for whoever runs next.
// =============================================================================

#include <glad/gl.h>

namespace OloEngine::Utils
{
    namespace Detail
    {
        // One pixel-store integer, set to 1 for the scope and restored after.
        template <GLenum Parameter>
        class GLAlignmentScope
        {
          public:
            GLAlignmentScope()
            {
                glGetIntegerv(Parameter, &m_Previous);
                if (m_Previous != 1)
                {
                    glPixelStorei(Parameter, 1);
                }
            }

            ~GLAlignmentScope()
            {
                if (m_Previous != 1)
                {
                    glPixelStorei(Parameter, m_Previous);
                }
            }

            GLAlignmentScope(const GLAlignmentScope&) = delete;
            GLAlignmentScope& operator=(const GLAlignmentScope&) = delete;
            GLAlignmentScope(GLAlignmentScope&&) = delete;
            GLAlignmentScope& operator=(GLAlignmentScope&&) = delete;

          private:
            GLint m_Previous = 4;
        };
    } // namespace Detail

    // Construct immediately before a glGetTextureImage / glGetTextureSubImage
    // whose DESTINATION buffer is tightly packed.
    using GLPackAlignmentScope = Detail::GLAlignmentScope<GL_PACK_ALIGNMENT>;

    // Construct immediately before a glTextureSubImage* whose SOURCE buffer is
    // tightly packed.
    using GLUnpackAlignmentScope = Detail::GLAlignmentScope<GL_UNPACK_ALIGNMENT>;
} // namespace OloEngine::Utils
