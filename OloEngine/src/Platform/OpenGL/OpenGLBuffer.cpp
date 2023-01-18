// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
	OpenGLVertexBuffer::OpenGLVertexBuffer(const uint32_t size)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
	}

	OpenGLVertexBuffer::OpenGLVertexBuffer(const uint32_t size, const GLenum usage)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, size, nullptr, usage);
	}

	OpenGLVertexBuffer::OpenGLVertexBuffer(const float* const vertices, const uint32_t size)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, vertices, GL_STATIC_DRAW);
	}

	OpenGLVertexBuffer::OpenGLVertexBuffer(const float* vertices, const uint32_t size, const GLenum usage)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, size, vertices, usage);
	}

	OpenGLVertexBuffer::~OpenGLVertexBuffer()
	{
		OLO_PROFILE_FUNCTION();

		glDeleteBuffers(1, &m_RendererID);
	}

	void OpenGLVertexBuffer::Bind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
	}

	void OpenGLVertexBuffer::Unbind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	void OpenGLVertexBuffer::SetData(const VertexData& data)
	{
		OLO_PROFILE_FUNCTION();

		glNamedBufferSubData(m_RendererID, 0, data.size, data.data);
	}


	OpenGLIndexBuffer::OpenGLIndexBuffer(uint32_t const* const indices, const uint32_t count)
		: m_Count(count)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, count * sizeof(uint32_t), indices, GL_STATIC_DRAW);
	}

	OpenGLIndexBuffer::OpenGLIndexBuffer(uint32_t const* indices, uint32_t count, GLenum usage)
		: m_Count(count)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, count * sizeof(uint32_t), indices, usage);
	}

	OpenGLIndexBuffer::~OpenGLIndexBuffer()
	{
		OLO_PROFILE_FUNCTION();

		glDeleteBuffers(1, &m_RendererID);
	}

	void OpenGLIndexBuffer::Bind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
	}

	void OpenGLIndexBuffer::Unbind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

	OpenGLUniformBuffer::OpenGLUniformBuffer(const uint32_t size, const uint32_t binding)
	{
		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
		glBindBufferBase(GL_UNIFORM_BUFFER, binding, m_RendererID);
	}

	OpenGLUniformBuffer::OpenGLUniformBuffer(const uint32_t size, const uint32_t binding, const GLenum usage)
	{
		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, size, nullptr, usage);
		glBindBufferRange(GL_UNIFORM_BUFFER, binding, m_RendererID, 0, size);
	}

	OpenGLUniformBuffer::~OpenGLUniformBuffer()
	{
		glDeleteBuffers(1, &m_RendererID);
	}

	void OpenGLUniformBuffer::SetData(const UniformData& data)
	{
		glNamedBufferSubData(m_RendererID, data.offset, data.size, data.data);
	}
}
