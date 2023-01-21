// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTextureCubemap.h"

#include <stb_image/stb_image.h>

namespace OloEngine
{

	OpenGLTextureCubemap::OpenGLTextureCubemap(const std::string& path)
	{
		OLO_PROFILE_FUNCTION();

		int width {};
		int height {};
		int channels {};
		::stbi_set_flip_vertically_on_load(1);
		stbi_uc* data = nullptr;
		{
			OLO_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
			data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
		}

		InvalidateImpl(path, static_cast<uint32_t>(width), static_cast<uint32_t>(height), data, static_cast<uint32_t>(channels));

		::stbi_image_free(data);
	}

	OpenGLTextureCubemap::~OpenGLTextureCubemap()
	{
		OLO_PROFILE_FUNCTION();

		glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLTextureCubemap::SetData(void* const data, const uint32_t size)
	{
		OLO_PROFILE_FUNCTION();

		const uint32_t bpp = GL_RGBA == m_DataFormat ? 4 : 3;
		OLO_CORE_ASSERT(size == m_Width * m_Height * bpp, "Data must be entire texture!");
		glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), m_DataFormat, GL_UNSIGNED_BYTE, data);
	}

	void OpenGLTextureCubemap::Invalidate(std::string_view path, uint32_t width, uint32_t height, const void* data, uint32_t channels)
	{
		OLO_PROFILE_FUNCTION();

		InvalidateImpl(path, width, height, data, channels);
	}

	void OpenGLTextureCubemap::InvalidateImpl(std::string_view path, uint32_t width, uint32_t height, const void* data, uint32_t channels)
	{
		OLO_PROFILE_FUNCTION();

		m_Path = path;

		if (data)
		{
			m_IsLoaded = true;

			m_Width = width;
			m_Height = height;

			GLenum internalFormat = 0;
			GLenum dataFormat = 0;
			// TODO(olbu): Add more channels?
			if (4 == channels)
			{
				internalFormat = GL_RGBA8;
				dataFormat = GL_RGBA;
			}
			else if (3 == channels)
			{
				internalFormat = GL_RGB8;
				dataFormat = GL_RGB;
			}

			m_InternalFormat = internalFormat;
			m_DataFormat = dataFormat;

			OLO_CORE_ASSERT(internalFormat & dataFormat, "Format not supported!");

			glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_RendererID);
			glTextureStorage2D(m_RendererID, 1, internalFormat, static_cast<int>(m_Width), static_cast<int>(m_Height));

			// NOTE: Texture Wrapping
			glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
			glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			// NOTE: Texture Filtering
			glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

			for (int i = 0; i < 6; ++i)
			{
				glTextureSubImage3D(m_RendererID, 0, 0, 0, i, static_cast<int>(m_Width), static_cast<int>(m_Height), 1, dataFormat, GL_UNSIGNED_BYTE, data);
			}

			glGenerateTextureMipmap(m_RendererID);
		}
	}

	void OpenGLTextureCubemap::Bind(const uint32_t slot) const
	{
		OLO_PROFILE_FUNCTION();

		glBindTextureUnit(slot, m_RendererID);
	}
}
