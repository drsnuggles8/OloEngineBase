#pragma once

#include "OloEngine/Renderer/Buffer.h"

namespace OloEngine {

	class OpenGLVertexBuffer : public VertexBuffer
	{
	public:
		explicit OpenGLVertexBuffer(uint32_t size);
		OpenGLVertexBuffer(const float* vertices, uint32_t size);
		~OpenGLVertexBuffer() override;

		void Bind() const override;
		void Unbind() const override;

		void SetData(void const* data, uint32_t size) override;

		[[nodiscard("This returns m_Layout, you probably wanted another function!")]] const BufferLayout& GetLayout() const override { return m_Layout; }
		void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }
	private:
		uint32_t m_RendererID{};
		BufferLayout m_Layout;
	};

	class OpenGLIndexBuffer : public IndexBuffer
	{
	public:
		OpenGLIndexBuffer(uint32_t const* indices, uint32_t count);
		~OpenGLIndexBuffer() override;

		void Bind() const override;
		void Unbind() const override;

		[[nodiscard("This returns m_Count, you probably wanted another function!")]] uint32_t GetCount() const override { return m_Count; }
	private:
		uint32_t m_RendererID{};
		uint32_t m_Count;
	};

}
