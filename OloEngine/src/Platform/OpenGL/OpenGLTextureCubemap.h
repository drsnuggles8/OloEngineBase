#pragma once
#include "OloEngine/Renderer/TextureCubemap.h"
#include "Platform/OpenGL/OpenGLTexture.h"

#include <glad/gl.h>

namespace OloEngine
{	
	class OpenGLTextureCubemap : public TextureCubemap
	{
	public:
		explicit OpenGLTextureCubemap(const TextureSpecification& specification);
		explicit OpenGLTextureCubemap(const std::array<std::string, 6>& facePaths);
		explicit OpenGLTextureCubemap(const std::string& folderPath);
		~OpenGLTextureCubemap() override;

		const TextureSpecification& GetSpecification() const override { return m_Specification; }

		[[nodiscard("Store this!")]] u32 GetWidth() const override { return m_Width; }
		[[nodiscard("Store this!")]] u32 GetHeight() const override { return m_Height; }
		[[nodiscard("Store this!")]] u32 GetRendererID() const override { return m_RendererID; }
		[[nodiscard("Store this!")]] const std::string& GetPath() const override { return m_FolderPath; }

		void SetData(void* data, u32 size) override;
		void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;

		void Bind(u32 slot) const override;

		[[nodiscard("Store this!")]] bool IsLoaded() const override { return m_IsLoaded; }

	private:
		void LoadCubemap(const std::array<std::string, 6>& facePaths);
		void LoadCubemapFromFolder(const std::string& folderPath);
		
	private:
		TextureSpecification m_Specification;

		std::string m_FolderPath;
		std::array<std::string, 6> m_FacePaths;
		bool m_IsLoaded = false;
		u32 m_Width{};
		u32 m_Height{};
		u32 m_RendererID{};
		GLenum m_InternalFormat{};
		GLenum m_DataFormat{};
	};

}
