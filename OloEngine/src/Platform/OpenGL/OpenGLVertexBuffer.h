#pragma once

#include "OloEngine/Renderer/VertexBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
	class OpenGLVertexBuffer : public VertexBuffer
	{
	public:
		explicit OpenGLVertexBuffer(const u32 size);
		OpenGLVertexBuffer(const u32 size, const GLenum usage);
		OpenGLVertexBuffer(const f32* vertices, u32 size);
		OpenGLVertexBuffer(const f32* vertices, u32 size, GLenum usage);
		OpenGLVertexBuffer(const void* data, u32 size);
		OpenGLVertexBuffer(const void* data, u32 size, GLenum usage);
		~OpenGLVertexBuffer() override;

		void Bind() const override;
		void Unbind() const override;

		void SetData(const VertexData& data) override;

		[[nodiscard("Store this!")]] const BufferLayout& GetLayout() const override { return m_Layout; }
		void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }
		[[nodiscard("Store this!")]] u32 GetBufferHandle() const override { return m_RendererID; }

	private:
		u32 m_RendererID{};
		u32 m_Size{}; // Buffer size in bytes for memory tracking
		BufferLayout m_Layout;
	};
}
