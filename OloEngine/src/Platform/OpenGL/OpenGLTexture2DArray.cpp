#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture2DArray.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"

#include <glad/gl.h>

namespace OloEngine
{
    namespace
    {
        auto Texture2DArrayFormatToGL(Texture2DArrayFormat format) -> GLenum
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

        // Calculate mip levels
        GLsizei mipLevels = 1;
        if (spec.GenerateMipmaps)
        {
            mipLevels = static_cast<GLsizei>(std::floor(std::log2(std::max(m_Width, m_Height)))) + 1;
        }
        glTextureStorage3D(m_RendererID, mipLevels, internalFormat, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height), static_cast<GLsizei>(m_Layers));

        if (spec.GenerateMipmaps)
        {
            glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
        else
        {
            glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        }
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        const bool isDepthFormat = (spec.Format == Texture2DArrayFormat::DEPTH_COMPONENT32F);
        const GLenum wrapMode = isDepthFormat ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapMode));
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapMode));
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, static_cast<GLint>(wrapMode));

        if (isDepthFormat)
        {
            // White border so areas outside the shadow map read as "no shadow"
            constexpr std::array<float, 4> borderColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            glTextureParameterfv(m_RendererID, GL_TEXTURE_BORDER_COLOR, borderColor.data());
        }

        if (isDepthFormat && spec.DepthComparisonMode)
        {
            glTextureParameteri(m_RendererID, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTextureParameteri(m_RendererID, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        }

        // Track GPU memory allocation
        sizet bytesPerPixel = 4; // DEPTH_COMPONENT32F = 4, RGBA8 = 4
        if (spec.Format == Texture2DArrayFormat::RGBA16F)
        {
            bytesPerPixel = 8;
        }
        else if (spec.Format == Texture2DArrayFormat::RGBA32F)
        {
            bytesPerPixel = 16;
        }
        sizet textureMemory = static_cast<sizet>(m_Width) * m_Height * m_Layers * bytesPerPixel;
        OLO_TRACK_GPU_ALLOC(this,
                            textureMemory,
                            RendererMemoryTracker::ResourceType::Texture2D,
                            "OpenGL Texture2DArray");
    }

    OpenGLTexture2DArray::~OpenGLTexture2DArray()
    {
        OLO_PROFILE_FUNCTION();
        OLO_TRACK_DEALLOC(this);
        glDeleteTextures(1, &m_RendererID);
    }

    void OpenGLTexture2DArray::Bind(u32 slot) const
    {
        OLO_PROFILE_FUNCTION();
        glBindTextureUnit(slot, m_RendererID);
    }

    void OpenGLTexture2DArray::SetLayerData(u32 layer, const void* data, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(layer < m_Layers, "Layer index out of bounds");
        OLO_CORE_ASSERT(width == m_Width && height == m_Height, "Layer data dimensions must match array dimensions");

        GLenum dataFormat = GL_RGBA;
        GLenum dataType = GL_UNSIGNED_BYTE;

        switch (m_Specification.Format)
        {
            case Texture2DArrayFormat::RGBA8:
                dataFormat = GL_RGBA;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case Texture2DArrayFormat::RGBA16F:
            case Texture2DArrayFormat::RGBA32F:
                dataFormat = GL_RGBA;
                dataType = (m_Specification.Format == Texture2DArrayFormat::RGBA16F) ? GL_HALF_FLOAT : GL_FLOAT;
                break;
            default:
                OLO_CORE_ASSERT(false, "SetLayerData not supported for depth formats");
                return;
        }

        glTextureSubImage3D(
            m_RendererID,
            0,                                  // mip level 0
            0, 0, static_cast<GLint>(layer),    // x, y, layer offset
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            1,                                  // depth = 1 layer
            dataFormat,
            dataType,
            data);
    }

    void OpenGLTexture2DArray::GenerateMipmaps()
    {
        OLO_PROFILE_FUNCTION();
        glGenerateTextureMipmap(m_RendererID);
    }

    // Factory
    Ref<Texture2DArray> Texture2DArray::Create(const Texture2DArraySpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        return Ref<OpenGLTexture2DArray>::Create(spec);
    }
} // namespace OloEngine
