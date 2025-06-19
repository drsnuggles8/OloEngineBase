#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLVertexBuffer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glad/gl.h>

namespace OloEngine
{	OpenGLVertexBuffer::OpenGLVertexBuffer(const u32 size) : m_Size(size)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
				// Track GPU memory allocation
		OLO_TRACK_GPU_ALLOC(reinterpret_cast<void*>(static_cast<uintptr_t>(m_RendererID)), 
		                     size, 
		                     RendererMemoryTracker::ResourceType::VertexBuffer, 
		                     "OpenGL VertexBuffer (dynamic)");

		// Register with GPU Resource Inspector
		GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (dynamic)");
	}
	OpenGLVertexBuffer::OpenGLVertexBuffer(const u32 size, const GLenum usage) : m_Size(size)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, size, nullptr, usage);
		// Track GPU memory allocation
		OLO_TRACK_GPU_ALLOC(this, 
		                     size, 
		                     RendererMemoryTracker::ResourceType::VertexBuffer, 
		                     "OpenGL VertexBuffer (storage)");

		// Register with GPU Resource Inspector
		GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (storage)");
	}
	OpenGLVertexBuffer::OpenGLVertexBuffer(const f32* const vertices, const u32 size) : m_Size(size)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, vertices, GL_STATIC_DRAW);
		// Track GPU memory allocation
		OLO_TRACK_GPU_ALLOC(this, 
		                     size, 
		                     RendererMemoryTracker::ResourceType::VertexBuffer, 
		                     "OpenGL VertexBuffer (static)");

		// Register with GPU Resource Inspector
		GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (static)");
	}
	OpenGLVertexBuffer::OpenGLVertexBuffer(const f32* vertices, const u32 size, const GLenum usage) : m_Size(size)
	{
		OLO_PROFILE_FUNCTION();

		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, size, vertices, usage);
		// Track GPU memory allocation
		OLO_TRACK_GPU_ALLOC(this, 
		                     size, 
		                     RendererMemoryTracker::ResourceType::VertexBuffer, 
		                     "OpenGL VertexBuffer (static storage)");

		// Register with GPU Resource Inspector
		GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (static storage)");
	}	OpenGLVertexBuffer::~OpenGLVertexBuffer()
	{
		OLO_PROFILE_FUNCTION();
		// Track GPU memory deallocation
		OLO_TRACK_DEALLOC(this);
		
		// Unregister from GPU Resource Inspector
		GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);
		
		glDeleteBuffers(1, &m_RendererID);
	}
	void OpenGLVertexBuffer::Bind() const
	{
		OLO_PROFILE_FUNCTION();

		glBindBuffer(GL_ARRAY_BUFFER, m_RendererID);
		RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);
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
}
