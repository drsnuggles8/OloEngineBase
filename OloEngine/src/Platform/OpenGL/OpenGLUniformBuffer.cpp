// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLUniformBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
	OpenGLUniformBuffer::OpenGLUniformBuffer(const u32 size, const u32 binding)
	{
		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
		glBindBufferBase(GL_UNIFORM_BUFFER, binding, m_RendererID);
	}

	OpenGLUniformBuffer::OpenGLUniformBuffer(const u32 size, const u32 binding, const GLenum usage)
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
