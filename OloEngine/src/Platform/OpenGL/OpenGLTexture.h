#pragma once

#include "OloEngine/Renderer/Texture.h"

#include <glad/gl.h>

namespace OloEngine {

	class OpenGLTexture2D : public Texture2D
	{
	public:
		OpenGLTexture2D(uint32_t width, uint32_t height);
		explicit OpenGLTexture2D(const std::string& path);
		~OpenGLTexture2D() override;

		[[nodiscard("Store this!")]] uint32_t GetWidth() const override { return m_Width;  }
		[[nodiscard("Store this!")]] uint32_t GetHeight() const override { return m_Height; }
		[[nodiscard("Store this!")]] uint32_t GetRendererID() const override { return m_RendererID; }
		[[nodiscard("Store this!")]] const std::string& GetPath() const override { return m_Path; }

		void SetData(void* data, uint32_t size) override;

		void Bind(uint32_t slot) const override;

		[[nodiscard("Store this!")]] bool IsLoaded() const override { return m_IsLoaded; }

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
