#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture.h"

#define STB_IMAGE_IMPLEMENTATION
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

	OpenGLTexture2D::OpenGLTexture2D(const TextureSpecification& specification)
		: m_Specification(specification), m_Width(m_Specification.Width), m_Height(m_Specification.Height)
	{
		OLO_PROFILE_FUNCTION();

		m_InternalFormat = Utils::OloEngineImageFormatToGLInternalFormat(m_Specification.Format);
		m_DataFormat = Utils::OloEngineImageFormatToGLDataFormat(m_Specification.Format);

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		glTextureStorage2D(m_RendererID, 1, m_InternalFormat, static_cast<int>(m_Width), static_cast<int>(m_Height));

		// NOTE: Texture Wrapping
		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// NOTE: Texture Filtering
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}

	OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
	{
		OLO_PROFILE_FUNCTION();

		int width = 0;
        int height = 0;
        int channels = 0;
		::stbi_set_flip_vertically_on_load(1);
		stbi_uc* data = nullptr;
		{
			OLO_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
			data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
		}

		if (!data)
		{
			OLO_CORE_ERROR("Image texture data is null!");
			::stbi_image_free(data);
			return;
		}

		InvalidateImpl(path, static_cast<u32>(width), static_cast<u32>(height), data, static_cast<u32>(channels));

		::stbi_image_free(data);
	}

	OpenGLTexture2D::~OpenGLTexture2D()
	{
		OLO_PROFILE_FUNCTION();

		glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLTexture2D::SetData(void* const data, const u32 size)
	{
		OLO_PROFILE_FUNCTION();

		const u32 bpp = GL_RGBA == m_DataFormat ? 4 : 3;
		OLO_CORE_ASSERT(size == m_Width * m_Height * bpp, "Data must be entire texture!");
		glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), m_DataFormat, GL_UNSIGNED_BYTE, data);
	}

	void OpenGLTexture2D::Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
	{
		OLO_PROFILE_FUNCTION();

		InvalidateImpl(path, width, height, data, channels);
	}

	void OpenGLTexture2D::InvalidateImpl(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
	{
		OLO_PROFILE_FUNCTION();

		m_Path = path;
		m_IsLoaded = true;
		m_Width = width;
		m_Height = height;

		GLenum internalFormat = 0;
		GLenum dataFormat = 0;
		switch (channels)
		{
			case 1:
				internalFormat = GL_R8;
				dataFormat = GL_RED;
				break;
			case 2:
				internalFormat = GL_RG8;
				dataFormat = GL_RG;
				break;
			case 3:
				internalFormat = GL_RGB8;
				dataFormat = GL_RGB;
				break;
			case 4:
				internalFormat = GL_RGBA8;
				dataFormat = GL_RGBA;
				break;
			default:
				OLO_CORE_ERROR("Texture channel count is not within (1-4) range. Channel count: {}", channels);
				return;
		}

		m_InternalFormat = internalFormat;
		m_DataFormat = dataFormat;

		OLO_CORE_ASSERT(internalFormat & dataFormat, "Format not supported!");

		glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
		glTextureStorage2D(m_RendererID, 1, internalFormat, static_cast<int>(m_Width), static_cast<int>(m_Height));

		// NOTE: Texture Wrapping
		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		// NOTE: Texture Filtering
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

		glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), dataFormat, GL_UNSIGNED_BYTE, data);
		glGenerateTextureMipmap(m_RendererID);
	}

	void OpenGLTexture2D::Bind(const u32 slot) const
	{
		OLO_PROFILE_FUNCTION();

		glBindTextureUnit(slot, m_RendererID);
	}
}
