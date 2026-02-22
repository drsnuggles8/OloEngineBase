#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture2DArray.h"

#include <glad/gl.h>

namespace OloEngine
{
    namespace
    {
        GLenum Texture2DArrayFormatToGL(Texture2DArrayFormat format)
        {
            switch (format)
            {
                case Texture2DArrayFormat::DEPTH_COMPONENT32F:
                    return GL_DEPTH_COMPONENT32F;
                case Texture2DArrayFormat::RGBA8:
                    return GL_RGBA8;
                case Texture2DArrayFormat::RGBA16F:
                    return GL_RGBA16F;
                case Texture2DArrayFormat::RGBA32F:
                    return GL_RGBA32F;
            }
            OLO_CORE_ASSERT(false, "Unknown Texture2DArrayFormat");
            return 0;
        }
    } // namespace

    OpenGLTexture2DArray::OpenGLTexture2DArray(const Texture2DArraySpecification& spec)
        : m_Width(spec.Width), m_Height(spec.Height), m_Layers(spec.Layers), m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();

        GLenum internalFormat = Texture2DArrayFormatToGL(spec.Format);

        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_RendererID);
        glTextureStorage3D(m_RendererID, 1, internalFormat, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height), static_cast<GLsizei>(m_Layers));

        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        const bool isDepthFormat = (spec.Format == Texture2DArrayFormat::DEPTH_COMPONENT32F);
        const GLenum wrapMode = isDepthFormat ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapMode));
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapMode));
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, static_cast<GLint>(wrapMode));

        if (isDepthFormat)
        {
            // White border so areas outside the shadow map read as "no shadow"
            constexpr float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
            glTextureParameterfv(m_RendererID, GL_TEXTURE_BORDER_COLOR, borderColor);
        }

        if (spec.DepthComparisonMode)
        {
            glTextureParameteri(m_RendererID, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTextureParameteri(m_RendererID, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        }
    }

    OpenGLTexture2DArray::~OpenGLTexture2DArray()
    {
        OLO_PROFILE_FUNCTION();
        glDeleteTextures(1, &m_RendererID);
    }

    void OpenGLTexture2DArray::Bind(u32 slot) const
    {
        glBindTextureUnit(slot, m_RendererID);
    }

    // Factory
    Ref<Texture2DArray> Texture2DArray::Create(const Texture2DArraySpecification& spec)
    {
        return Ref<OpenGLTexture2DArray>::Create(spec);
    }
} // namespace OloEngine
