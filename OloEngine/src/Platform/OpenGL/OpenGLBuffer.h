#pragma once

#include "OloEngine/Renderer/Buffer.h"

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
		~OpenGLVertexBuffer() override;

		void Bind() const override;
		void Unbind() const override;

		void SetData(const VertexData& data) override;

		[[nodiscard("Store this!")]] const BufferLayout& GetLayout() const override { return m_Layout; }
		void SetLayout(const BufferLayout& layout) override { m_Layout = layout; }

		[[nodiscard("Store this!")]] u32 GetBufferHandle() const override { return m_RendererID; }
	private:
		u32 m_RendererID{};
		BufferLayout m_Layout;
	};

	class OpenGLIndexBuffer : public IndexBuffer
	{
	public:
		OpenGLIndexBuffer(u32 const* indices, u32 count);
		OpenGLIndexBuffer(u32 const* indices, u32 count, GLenum usage);
		~OpenGLIndexBuffer() override;

		void Bind() const override;
		void Unbind() const override;

		[[nodiscard("Store this!")]] u32 GetCount() const override { return m_Count; }
		[[nodiscard("Store this!")]] u32 GetBufferHandle() const override { return m_RendererID; }
	private:
		u32 m_RendererID{};
		u32 m_Count;
	};

	class OpenGLUniformBuffer : public UniformBuffer
	{
	public:
		OpenGLUniformBuffer(const u32 size, const u32 binding);
		OpenGLUniformBuffer(const u32 size, const u32 binding, const GLenum usage);
		~OpenGLUniformBuffer() override;

		void SetData(const UniformData& data) override;
	private:
		u32 m_RendererID = 0;
	};
}
