// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "OloEnginePCH.h"
#include "OpenGLUniformBuffer.h"

#include <glad/glad.h>

namespace OloEngine {

	OpenGLUniformBuffer::OpenGLUniformBuffer(const uint32_t size, const uint32_t binding)
	{
		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW); // TODO: investigate usage hint
		glBindBufferBase(GL_UNIFORM_BUFFER, binding, m_RendererID);
	}

	OpenGLUniformBuffer::~OpenGLUniformBuffer()
	{
		glDeleteBuffers(1, &m_RendererID);
	}


	void OpenGLUniformBuffer::SetData(const void* const data, const uint32_t size, const uint32_t offset)
	{
		glNamedBufferSubData(m_RendererID, offset, size, data);
	}

}
