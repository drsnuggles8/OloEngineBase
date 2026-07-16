#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture3D.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
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
                case Texture3DFormat::RGBA8:
                    return GL_RGBA8;
                case Texture3DFormat::RGBA16F:
                    return GL_RGBA16F;
                case Texture3DFormat::RGBA32F:
                    return GL_RGBA32F;
            }
            OLO_CORE_ASSERT(false, "Unknown Texture3DFormat");
            return 0;
        }

        auto Texture3DFormatBytesPerPixel(Texture3DFormat format) -> sizet
        {
            switch (format)
            {
                case Texture3DFormat::RGBA8:
                    return 4;
                case Texture3DFormat::RGBA16F:
                    return 8;
                case Texture3DFormat::RGBA32F:
                    return 16;
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

        // Trilinear filtering for smooth volume interpolation
        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Repeat for tiling noise volumes; clamp-to-edge otherwise (samples
        // outside the grid get the boundary value — e.g. wind fields)
        const GLint wrapMode = spec.Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, wrapMode);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, wrapMode);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, wrapMode);

        // Track GPU memory allocation
        sizet bytesPerPixel = Texture3DFormatBytesPerPixel(spec.Format);
        sizet textureMemory = static_cast<sizet>(m_Width) * m_Height * m_Depth * bytesPerPixel;
        OLO_TRACK_GPU_ALLOC(this,
                            textureMemory,
                            RendererMemoryTracker::ResourceType::Other,
                            "OpenGL Texture3D");
    }

    OpenGLTexture3D::~OpenGLTexture3D()
    {
        OLO_PROFILE_FUNCTION();
        if (m_RendererID != 0)
        {
            OLO_TRACK_DEALLOC(this);
        }

        u32 id = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([id]()
                                                      { glDeleteTextures(1, &id); });
    }

    void OpenGLTexture3D::Bind(u32 slot) const
    {
        OLO_PROFILE_FUNCTION();
        glBindTextureUnit(slot, m_RendererID);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TextureBinds, 1);
    }

    // Factory
    Ref<Texture3D> Texture3D::Create(const Texture3DSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        return Ref<OpenGLTexture3D>::Create(spec);
    }
} // namespace OloEngine
