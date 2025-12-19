#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "Platform/OpenGL/OpenGLFramebuffer.h"

#include <glad/gl.h>

#include <utility>

namespace OloEngine
{
    namespace Utils
    {
        [[nodiscard("Store this!")]] constexpr GLenum TextureTarget(const bool multisampled) noexcept
        {
            return multisampled ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
        }

        void PrepareTexture(const u32 id, const int samples, const GLenum format, const int width, const int height)
        {
            OLO_CORE_ASSERT((format == GL_RGBA8 || format == GL_RGBA16F || format == GL_RGBA32F ||
                             format == GL_RGB16F || format == GL_RGB32F || format == GL_RG16F ||
                             format == GL_RG32F || format == GL_R32I || format == GL_DEPTH24_STENCIL8),
                            "Invalid format.");

            if (const bool multisampled = samples > 1)
            {
                glTextureStorage2DMultisample(id, samples, format, width, height, GL_FALSE);
            }
            else
            {
                glTextureStorage2D(id, 1, format, width, height);
                glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTextureParameteri(id, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
                glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }

        void CreateTextures(const bool multisampled, const int count, u32* const outID)
        {
            glCreateTextures(TextureTarget(multisampled), count, outID);
        }

        void BindTexture(const u32 id)
        {
            glBindTextureUnit(0, id);
        }

        void BindTextures(const u32 firstID, const u32 count, const GLuint* id)
        {
            glBindTextures(firstID, static_cast<int>(count), id);
        }

        void AttachColorTexture(const u32 fbo, const u32 id, const int samples, const GLenum internalFormat, const int width, const int height, const u32 index)
        {
            PrepareTexture(id, samples, internalFormat, width, height);

            glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + index, id, 0);

            auto fboStatus = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
            if (fboStatus != GL_FRAMEBUFFER_COMPLETE)
            {
                OLO_CORE_ERROR("Framebuffer error: {0}", fboStatus);
            }
        }

        void AttachDepthTexture(const u32 fbo, const u32 id, const int samples, const GLenum format, const GLenum attachmentType, const int width, const int height)
        {
            PrepareTexture(id, samples, format, width, height);

            glNamedFramebufferTexture(fbo, attachmentType, id, 0);

            auto fboStatus = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
            if (fboStatus != GL_FRAMEBUFFER_COMPLETE)
            {
                OLO_CORE_ERROR("Framebuffer error: {0}", fboStatus);
            }
        }

        [[nodiscard("Store this!")]] bool IsDepthFormat(const FramebufferTextureFormat format) noexcept
        {
            switch (format)
            {
                case FramebufferTextureFormat::DEPTH24STENCIL8:
                    return true;
            }
            return false;
        }

        [[nodiscard("Store this!")]] GLenum OloFBTextureFormatToGL(const FramebufferTextureFormat format)
        {
            switch (format)
            {
                case FramebufferTextureFormat::RGBA8:
                    return GL_RGBA8;
                case FramebufferTextureFormat::RGBA16F:
                    return GL_RGBA16F;
                case FramebufferTextureFormat::RGBA32F:
                    return GL_RGBA32F;
                case FramebufferTextureFormat::RGB16F:
                    return GL_RGB16F;
                case FramebufferTextureFormat::RGB32F:
                    return GL_RGB32F;
                case FramebufferTextureFormat::RG16F:
                    return GL_RG16F;
                case FramebufferTextureFormat::RG32F:
                    return GL_RG32F;
                case FramebufferTextureFormat::RED_INTEGER:
                    return GL_RED_INTEGER;
            }

            OLO_CORE_ASSERT(false);
            return 0;
        }

        [[nodiscard("Store this!")]] GLenum OloFBColorTextureFormatToGL(const FramebufferTextureFormat format)
        {
            switch (format)
            {
                case FramebufferTextureFormat::RGBA8:
                    return GL_RGBA8;
                case FramebufferTextureFormat::RGBA16F:
                    return GL_RGBA16F;
                case FramebufferTextureFormat::RGBA32F:
                    return GL_RGBA32F;
                case FramebufferTextureFormat::RGB16F:
                    return GL_RGB16F;
                case FramebufferTextureFormat::RGB32F:
                    return GL_RGB32F;
                case FramebufferTextureFormat::RG16F:
                    return GL_RG16F;
                case FramebufferTextureFormat::RG32F:
                    return GL_RG32F;
                case FramebufferTextureFormat::RED_INTEGER:
                    return GL_R32I;
            }

            OLO_CORE_ASSERT(false);
            return 0;
        }

        [[nodiscard("Store this!")]] GLenum OloFBDepthTextureFormatToGL(const FramebufferTextureFormat format)
        {
            switch (format)
            {
                case FramebufferTextureFormat::DEPTH24STENCIL8:
                    return GL_DEPTH24_STENCIL8;
            }

            OLO_CORE_ASSERT(false);
            return 0;
        }
    } // namespace Utils

} // namespace OloEngine
