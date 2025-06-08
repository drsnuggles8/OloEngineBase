#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTextureCubemap.h"

#include <stb_image/stb_image.h>

namespace OloEngine
{
    namespace Utils
    {
        [[nodiscard("Store this!")]] static GLenum OloEngineImageFormatToGLDataFormat(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8: return GL_RED;
                case ImageFormat::RGB8: return GL_RGB;
                case ImageFormat::RGBA8: return GL_RGBA;
                case ImageFormat::R32F: return GL_RED;
                case ImageFormat::RG32F: return GL_RG;
                case ImageFormat::RGB32F: return GL_RGB;
                case ImageFormat::RGBA32F: return GL_RGBA;
                case ImageFormat::DEPTH24STENCIL8: return GL_DEPTH_STENCIL;
            }

            OLO_CORE_ASSERT(false, "Unknown ImageFormat!");
            return 0;
        }

        [[nodiscard("Store this!")]] static GLenum OloEngineImageFormatToGLInternalFormat(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8: return GL_R8;
                case ImageFormat::RGB8: return GL_RGB8;
                case ImageFormat::RGBA8: return GL_RGBA8;
                case ImageFormat::R32F: return GL_R32F;
                case ImageFormat::RG32F: return GL_RG32F;
                case ImageFormat::RGB32F: return GL_RGB32F;
                case ImageFormat::RGBA32F: return GL_RGBA32F;
                case ImageFormat::DEPTH24STENCIL8: return GL_DEPTH24_STENCIL8;
            }

            OLO_CORE_ASSERT(false, "Unknown ImageFormat!");
            return 0;
        }
    }

    OpenGLTextureCubemap::OpenGLTextureCubemap(const CubemapSpecification& specification)
        : m_CubemapSpecification(specification), m_Width(m_CubemapSpecification.Width), m_Height(m_CubemapSpecification.Height)
    {
        OLO_PROFILE_FUNCTION();

        // Set up TextureSpecification from CubemapSpecification
        m_Specification.Width = m_CubemapSpecification.Width;
        m_Specification.Height = m_CubemapSpecification.Height;
        m_Specification.Format = m_CubemapSpecification.Format;
        m_Specification.GenerateMips = m_CubemapSpecification.GenerateMips;

        m_InternalFormat = Utils::OloEngineImageFormatToGLInternalFormat(m_CubemapSpecification.Format);
        m_DataFormat = Utils::OloEngineImageFormatToGLDataFormat(m_CubemapSpecification.Format);

        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_RendererID);
        glTextureStorage2D(m_RendererID, m_CubemapSpecification.GenerateMips ? 4 : 1, m_InternalFormat, 
                          static_cast<int>(m_Width), static_cast<int>(m_Height));

        // Set texture parameters appropriate for cubemaps
        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, m_CubemapSpecification.GenerateMips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        m_IsLoaded = true;
        m_Path = "Generated Cubemap";
    }

    OpenGLTextureCubemap::OpenGLTextureCubemap(const std::vector<std::string>& facePaths)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(facePaths.size() == 6, "Cubemap must have exactly 6 face textures!");
		m_HasAlphaChannel = false;
        
        m_Path = facePaths[0] + ",..."; // Store abbreviated path of the first face + indication of more
        LoadFaces(facePaths);
    }

    OpenGLTextureCubemap::~OpenGLTextureCubemap()
    {
        OLO_PROFILE_FUNCTION();

        glDeleteTextures(1, &m_RendererID);
    }

    void OpenGLTextureCubemap::LoadFaces(const std::vector<std::string>& facePaths)
    {
        OLO_PROFILE_FUNCTION();

        glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_RendererID);
        
        int width;
		int height;
		int channels;
        stbi_set_flip_vertically_on_load(0); // Don't flip cubemap textures

        // First pass: check sizes and determine format
        bool firstFace = true;
        for (u32 i = 0; i < 6; i++)
        {
            stbi_uc* data = stbi_load(facePaths[i].c_str(), &width, &height, &channels, 0);
            if (!data)
            {
                OLO_CORE_ERROR("Failed to load cubemap face {}: {}", i, facePaths[i]);
                stbi_image_free(data);
                return;
            }

            // Check if all images have the same dimensions
            if (firstFace)
            {
                m_Width = static_cast<u32>(width);
                m_Height = static_cast<u32>(height);
                firstFace = false;

                // Set up the texture format based on the first face
                switch (channels)
                {
                    case 1: 
                        m_InternalFormat = GL_R8; 
                        m_DataFormat = GL_RED; 
                        break;
                    case 3: 
                        m_InternalFormat = GL_RGB8; 
                        m_DataFormat = GL_RGB; 
                        break;
                    case 4: 
                        m_InternalFormat = GL_RGBA8; 
                        m_DataFormat = GL_RGBA;
						m_HasAlphaChannel = true;
                        break;
                    default:
                        OLO_CORE_ERROR("Unsupported number of channels for cubemap texture: {}", channels);
                        stbi_image_free(data);
                        return;
                }

                // Set up specifications
                m_CubemapSpecification.Width = m_Width;
                m_CubemapSpecification.Height = m_Height;
                m_CubemapSpecification.Format = channels == 1 ? ImageFormat::R8 : 
                                              (channels == 3 ? ImageFormat::RGB8 : ImageFormat::RGBA8);
                m_CubemapSpecification.GenerateMips = true;

                m_Specification.Width = m_Width;
                m_Specification.Height = m_Height;
                m_Specification.Format = m_CubemapSpecification.Format;
                m_Specification.GenerateMips = true;

                // Allocate storage for the cubemap
                glTextureStorage2D(m_RendererID, 4, m_InternalFormat, width, height);
            }
            else if (static_cast<u32>(width) != m_Width || static_cast<u32>(height) != m_Height)
            {
                OLO_CORE_ERROR("Cubemap face {} has inconsistent dimensions", i);
                stbi_image_free(data);
                return;
            }

            stbi_image_free(data);
        }

        // Second pass: load and upload each face
        for (u32 i = 0; i < 6; i++)
        {
            stbi_uc* data = stbi_load(facePaths[i].c_str(), &width, &height, &channels, 0);
            if (!data)
            {
                OLO_CORE_ERROR("Failed to load cubemap face {}: {}", i, facePaths[i]);
                continue;
            }

            glTextureSubImage3D(
                m_RendererID, 
                0,                          // Mipmap level
                0, 0,                       // X and Y offsets
                i,                          // Face index (Z offset in 3D texture terms)
                static_cast<int>(m_Width),  // Width
                static_cast<int>(m_Height), // Height
                1,                          // Depth (1 for a single face)
                m_DataFormat,               // Format of the pixel data
                GL_UNSIGNED_BYTE,           // Data type
                data                        // Pixel data
            );

            stbi_image_free(data);
        }

        // Set texture parameters appropriate for cubemaps
        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        // Generate mipmaps
        glGenerateTextureMipmap(m_RendererID);

        m_IsLoaded = true;
        OLO_CORE_TRACE("Loaded cubemap with {} faces, dimensions: {}x{}", facePaths.size(), m_Width, m_Height);
    }

    void OpenGLTextureCubemap::SetData(void* /*data*/, u32 /*size*/)
    {
        OLO_PROFILE_FUNCTION();
        
        OLO_CORE_ERROR("SetData is not supported for cubemaps, use SetFaceData instead");
    }

    void OpenGLTextureCubemap::SetFaceData(u32 faceIndex, void* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(faceIndex < 6, "Face index out of range! Must be 0-5.");
        const u32 bpp = m_DataFormat == GL_RGBA ? 4 : (m_DataFormat == GL_RGB ? 3 : 1);
        OLO_CORE_ASSERT(size == m_Width * m_Height * bpp, "Data size doesn't match face dimensions!");

        glTextureSubImage3D(
            m_RendererID, 
            0,                          // Mipmap level
            0, 0,                       // X and Y offsets
            faceIndex,                  // Face index (Z offset in 3D texture terms)
            static_cast<int>(m_Width),  // Width
            static_cast<int>(m_Height), // Height
            1,                          // Depth (1 for a single face)
            m_DataFormat,               // Format of the pixel data
            GL_UNSIGNED_BYTE,           // Data type
            data                        // Pixel data
        );

        if (m_CubemapSpecification.GenerateMips)
            glGenerateTextureMipmap(m_RendererID);
    }

    void OpenGLTextureCubemap::Invalidate(std::string_view /*path*/, u32 /*width*/, u32 /*height*/, const void* /*data*/, u32 /*channels*/)
    {
        OLO_CORE_ERROR("Invalidate is not supported for cubemaps");
    }

    void OpenGLTextureCubemap::Bind(const u32 slot) const
    {
        OLO_PROFILE_FUNCTION();

        glBindTextureUnit(slot, m_RendererID);
    }
}
