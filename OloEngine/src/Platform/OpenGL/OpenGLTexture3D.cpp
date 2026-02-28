#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture3D.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Renderer.h"

#include <glad/gl.h>

namespace OloEngine
{
    namespace
    {
        auto Texture3DFormatToGL(Texture3DFormat format) -> GLenum
        {
            switch (format)
            {
                case Texture3DFormat::RGBA16F:
                    return GL_RGBA16F;
                case Texture3DFormat::RGBA32F:
                    return GL_RGBA32F;
            }
            OLO_CORE_ASSERT(false, "Unknown Texture3DFormat");
            return 0;
        }
    } // namespace

    OpenGLTexture3D::OpenGLTexture3D(const Texture3DSpecification& spec)
        : m_Width(spec.Width), m_Height(spec.Height), m_Depth(spec.Depth), m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Width == 0 || m_Height == 0 || m_Depth == 0)
        {
            OLO_CORE_ERROR("OpenGLTexture3D: Invalid dimensions ({}x{}x{}) — all must be > 0",
                           m_Width, m_Height, m_Depth);
            return;
        }

        GLenum internalFormat = Texture3DFormatToGL(spec.Format);

        glCreateTextures(GL_TEXTURE_3D, 1, &m_RendererID);
        glTextureStorage3D(m_RendererID, 1, internalFormat,
                           static_cast<GLsizei>(m_Width),
                           static_cast<GLsizei>(m_Height),
                           static_cast<GLsizei>(m_Depth));

        // Trilinear filtering for smooth wind field interpolation
        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Clamp to edge — particles outside the grid get the boundary wind value
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        // Track GPU memory allocation
        sizet bytesPerPixel = (spec.Format == Texture3DFormat::RGBA16F) ? 8 : 16;
        sizet textureMemory = static_cast<sizet>(m_Width) * m_Height * m_Depth * bytesPerPixel;
        OLO_TRACK_GPU_ALLOC(this,
                            textureMemory,
                            RendererMemoryTracker::ResourceType::Texture2D,
                            "OpenGL Texture3D (Wind Field)");
    }

    OpenGLTexture3D::~OpenGLTexture3D()
    {
        OLO_PROFILE_FUNCTION();
        OLO_TRACK_DEALLOC(this);
        glDeleteTextures(1, &m_RendererID);
    }

    void OpenGLTexture3D::Bind(u32 slot) const
    {
        OLO_PROFILE_FUNCTION();
        glBindTextureUnit(slot, m_RendererID);
    }

    // Factory
    Ref<Texture3D> Texture3D::Create(const Texture3DSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        return Ref<OpenGLTexture3D>::Create(spec);
    }
} // namespace OloEngine
