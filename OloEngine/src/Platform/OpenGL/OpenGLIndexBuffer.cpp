#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLIndexBuffer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"

#include <glad/gl.h>

namespace OloEngine
{	OpenGLIndexBuffer::OpenGLIndexBuffer(u32 const* const indices, const u32 count)
		: m_Count(count)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, count * sizeof(u32), indices, GL_STATIC_DRAW);
				// Track GPU memory allocation
		u32 bufferSize = count * sizeof(u32);
		OLO_TRACK_GPU_ALLOC(this, 
		                     bufferSize, 
		                     RendererMemoryTracker::ResourceType::IndexBuffer, 
		                     "OpenGL IndexBuffer (static)");
	}
	OpenGLIndexBuffer::OpenGLIndexBuffer(u32 const* indices, u32 count, GLenum usage)
		: m_Count(count)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, count * sizeof(u32), indices, usage);
				// Track GPU memory allocation
		u32 bufferSize = count * sizeof(u32);
		OLO_TRACK_GPU_ALLOC(this, 
		                     bufferSize, 
		                     RendererMemoryTracker::ResourceType::IndexBuffer, 
		                     "OpenGL IndexBuffer (storage)");
	}
	OpenGLIndexBuffer::~OpenGLIndexBuffer()
	{
		OLO_PROFILE_FUNCTION();
		// Track GPU memory deallocation
		OLO_TRACK_DEALLOC(this);
		
		glDeleteBuffers(1, &m_RendererID);
	}
	void OpenGLIndexBuffer::Bind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_RendererID);
		RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);
	}

	void OpenGLIndexBuffer::Unbind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
}
