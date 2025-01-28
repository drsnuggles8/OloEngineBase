#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLIndexBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
	OpenGLIndexBuffer::OpenGLIndexBuffer(u32 const* const indices, const u32 count)
		: m_Count(count)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, count * sizeof(u32), indices, GL_STATIC_DRAW);
	}

	OpenGLIndexBuffer::OpenGLIndexBuffer(u32 const* indices, u32 count, GLenum usage)
		: m_Count(count)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, count * sizeof(u32), indices, usage);
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
}
