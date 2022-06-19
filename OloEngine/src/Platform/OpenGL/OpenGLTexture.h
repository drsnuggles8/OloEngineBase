#pragma once

#include "OloEngine/Renderer/Texture.h"

#include <glad/glad.h>

namespace OloEngine {

	class OpenGLTexture2D : public Texture2D
	{
	public:
		OpenGLTexture2D(uint32_t width, uint32_t height);
		OpenGLTexture2D(const std::string& path);
		~OpenGLTexture2D() override;

		[[nodiscard("This returns m_Width, you probably wanted another function!")]] uint32_t GetWidth() const override { return m_Width;  }
		[[nodiscard("This returns m_Height, you probably wanted another function!")]] uint32_t GetHeight() const override { return m_Height; }
		[[nodiscard("This returns m_RendererID, you probably wanted another function!")]] uint32_t GetRendererID() const override { return m_RendererID; }

		void SetData(void* data, uint32_t size) override;

		void Bind(uint32_t slot) const override;

		[[nodiscard("This returns m_IsLoaded, you probably wanted another function!")]] bool IsLoaded() const override { return m_IsLoaded; }

		bool operator==(const Texture& other) const override
		{
			return m_RendererID == other.GetRendererID();
		}
	private:
		std::string m_Path;
		bool m_IsLoaded = false;
		uint32_t m_Width;
		uint32_t m_Height;
		uint32_t m_RendererID{};
		GLenum m_InternalFormat;
		GLenum m_DataFormat;
	};

}
