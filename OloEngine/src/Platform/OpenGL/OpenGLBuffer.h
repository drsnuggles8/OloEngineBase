#pragma once

#include "OloEngine/Renderer/Buffer.h"

#include <glad/gl.h>

namespace OloEngine
{
	class OpenGLVertexBuffer : public VertexBuffer
	{
	public:
		explicit OpenGLVertexBuffer(const uint32_t size);
		OpenGLVertexBuffer(const uint32_t size, const GLenum usage);
		OpenGLVertexBuffer(const float* vertices, uint32_t size);
		OpenGLVertexBuffer(const float* vertices, uint32_t size, GLenum usage);
		~OpenGLVertexBuffer() override;

		void Bind() const override;
		void Unbind() const override;

		void SetData(const VertexData& data) override;

		[[nodiscard("Store this!")]] const BufferLayout& GetLayout() const override { return m_Layout; }
		void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }

		[[nodiscard("Store this!")]] uint32_t GetBufferHandle() const override { return m_RendererID; }
	private:
		uint32_t m_RendererID{};
		BufferLayout m_Layout;
	};

	class OpenGLIndexBuffer : public IndexBuffer
	{
	public:
		OpenGLIndexBuffer(uint32_t const* indices, uint32_t count);
		OpenGLIndexBuffer(uint32_t const* indices, uint32_t count, GLenum usage);
		~OpenGLIndexBuffer() override;

		void Bind() const override;
		void Unbind() const override;

		[[nodiscard("Store this!")]] uint32_t GetCount() const override { return m_Count; }
		[[nodiscard("Store this!")]] uint32_t GetBufferHandle() const override { return m_RendererID; }
	private:
		uint32_t m_RendererID{};
		uint32_t m_Count;
	};

	class OpenGLUniformBuffer : public UniformBuffer
	{
	public:
		OpenGLUniformBuffer(const uint32_t size, const uint32_t binding);
		OpenGLUniformBuffer(const uint32_t size, const uint32_t binding, const GLenum usage);
		~OpenGLUniformBuffer() override;

		void SetData(const UniformData& data) override;
	private:
		uint32_t m_RendererID = 0;
	};
}
