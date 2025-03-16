#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexturecubemap.h"

#include <stb_image/stb_image.h>

#include <filesystem>

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

	// OpenGLTextureCubemap Implementation
	OpenGLTextureCubemap::OpenGLTextureCubemap(const TextureSpecification& specification)
		: m_Specification(specification), m_Width(m_Specification.Width), m_Height(m_Specification.Height)
	{
		OLO_PROFILE_FUNCTION();

		m_InternalFormat = Utils::OloEngineImageFormatToGLInternalFormat(m_Specification.Format);
		m_DataFormat = Utils::OloEngineImageFormatToGLDataFormat(m_Specification.Format);

		glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_RendererID);
		glTextureStorage2D(m_RendererID, 1, m_InternalFormat, static_cast<int>(m_Width), static_cast<int>(m_Height));

		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	}

	OpenGLTextureCubemap::OpenGLTextureCubemap(const std::array<std::string, 6>& facePaths)
		: m_FacePaths(facePaths)
	{
		OLO_PROFILE_FUNCTION();
		LoadCubemap(facePaths);
	}

	OpenGLTextureCubemap::OpenGLTextureCubemap(const std::string& folderPath)
		: m_FolderPath(folderPath)
	{
		OLO_PROFILE_FUNCTION();
		LoadCubemapFromFolder(folderPath);
	}

	OpenGLTextureCubemap::~OpenGLTextureCubemap()
	{
		OLO_PROFILE_FUNCTION();
		glDeleteTextures(1, &m_RendererID);
	}

	void OpenGLTextureCubemap::SetData(void* data, u32 size)
	{
		OLO_PROFILE_FUNCTION();
		
		// Not fully implemented - would need to specify which face to update
		OLO_CORE_WARN("OpenGLTextureCubemap::SetData - Not fully implemented!");
	}

	void OpenGLTextureCubemap::Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
	{
		OLO_PROFILE_FUNCTION();
		
		// Not implemented for cubemaps - use LoadCubemap or LoadCubemapFromFolder instead
		OLO_CORE_WARN("OpenGLTextureCubemap::Invalidate - Not implemented for cubemaps!");
	}

	void OpenGLTextureCubemap::Bind(u32 slot) const
	{
		OLO_PROFILE_FUNCTION();

		glBindTextureUnit(slot, m_RendererID);
	}

	void OpenGLTextureCubemap::LoadCubemap(const std::array<std::string, 6>& facePaths)
	{
		OLO_PROFILE_FUNCTION();

		// Create cubemap texture
		glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_RendererID);
		
		// Order of faces for OpenGL cubemaps:
		// GL_TEXTURE_CUBE_MAP_POSITIVE_X = Right
		// GL_TEXTURE_CUBE_MAP_NEGATIVE_X = Left
		// GL_TEXTURE_CUBE_MAP_POSITIVE_Y = Top
		// GL_TEXTURE_CUBE_MAP_NEGATIVE_Y = Bottom
		// GL_TEXTURE_CUBE_MAP_POSITIVE_Z = Front
		// GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = Back

		int width, height, channels;
		
		// Don't flip textures for cubemaps
		::stbi_set_flip_vertically_on_load(0);
		
		// Load the first image to determine size and format
		unsigned char* data = ::stbi_load(facePaths[0].c_str(), &width, &height, &channels, 0);
		if (!data)
		{
			OLO_CORE_ERROR("Failed to load cubemap face: {}", facePaths[0]);
			return;
		}

		// Store dimensions
		m_Width = static_cast<u32>(width);
		m_Height = static_cast<u32>(height);

		// Setup internal format and data format based on channels
		GLenum internalFormat = 0, dataFormat = 0;
		switch (channels)
		{
			case 3:
				internalFormat = GL_RGB8;
				dataFormat = GL_RGB;
				break;
			case 4:
				internalFormat = GL_RGBA8;
				dataFormat = GL_RGBA;
				break;
			default:
				OLO_CORE_ERROR("Unsupported number of channels for cubemap: {}", channels);
				::stbi_image_free(data);
				return;
		}

		m_InternalFormat = internalFormat;
		m_DataFormat = dataFormat;

		// Allocate storage for all faces of the same size
		glTextureStorage2D(m_RendererID, 1, internalFormat, width, height);
		
		// Upload data for the first face
		glTextureSubImage3D(m_RendererID, 0, 0, 0, 0, width, height, 1, dataFormat, GL_UNSIGNED_BYTE, data);
		::stbi_image_free(data);

		// Load and upload the remaining faces
		for (u32 i = 1; i < 6; i++)
		{
			data = ::stbi_load(facePaths[i].c_str(), &width, &height, &channels, 0);
			if (!data)
			{
				OLO_CORE_ERROR("Failed to load cubemap face: {}", facePaths[i]);
				continue;
			}

			glTextureSubImage3D(m_RendererID, 0, 0, 0, i, width, height, 1, dataFormat, GL_UNSIGNED_BYTE, data);
			::stbi_image_free(data);
		}

		// Set texture parameters
		glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		m_IsLoaded = true;
		OLO_CORE_INFO("Cubemap loaded successfully from 6 faces");
	}

	void OpenGLTextureCubemap::LoadCubemapFromFolder(const std::string& folderPath)
	{
		OLO_PROFILE_FUNCTION();

		// Standard naming for skybox faces
		static const std::array<std::string, 6> faceNames = {
			"right.jpg", "left.jpg",
			"top.jpg", "bottom.jpg",
			"front.jpg", "back.jpg"
		};

		// Create full paths
		std::array<std::string, 6> facePaths;
		for (u32 i = 0; i < 6; i++)
		{
			facePaths[i] = folderPath + "/" + faceNames[i];
			
			// Try alternative extensions if file doesn't exist
			if (!std::filesystem::exists(facePaths[i]))
			{
				// Try png instead
				facePaths[i] = folderPath + "/" + faceNames[i].substr(0, faceNames[i].length() - 4) + ".png";
				
				// If still doesn't exist, try other common names
				if (!std::filesystem::exists(facePaths[i]))
				{
					// Try posx, negx, etc. format
					std::string direction = "";
					switch (i)
					{
						case 0: direction = "posx"; break;
						case 1: direction = "negx"; break;
						case 2: direction = "posy"; break;
						case 3: direction = "negy"; break;
						case 4: direction = "posz"; break;
						case 5: direction = "negz"; break;
					}
					
					// Try jpg
					facePaths[i] = folderPath + "/" + direction + ".jpg";
					if (!std::filesystem::exists(facePaths[i]))
					{
						// Try png
						facePaths[i] = folderPath + "/" + direction + ".png";
					}
				}
			}
			
			OLO_CORE_INFO("Loading cubemap face: {}", facePaths[i]);
		}
		
		// Store folder path
		m_FolderPath = folderPath;
		
		// Load the cubemap using the individual face loading method
		LoadCubemap(facePaths);
	}
}
