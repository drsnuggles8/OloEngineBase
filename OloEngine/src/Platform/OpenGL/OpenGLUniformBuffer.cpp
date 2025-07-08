#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLUniformBuffer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glad/gl.h>

namespace OloEngine
{
	
	OpenGLUniformBuffer::OpenGLUniformBuffer(const u32 size, const u32 binding)
	{
		glCreateBuffers(1, &m_RendererID);
		glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
		glBindBufferBase(GL_UNIFORM_BUFFER, binding, m_RendererID);
		RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);
		// Track GPU memory allocation
		OLO_TRACK_GPU_ALLOC(this, size, RendererMemoryTracker::ResourceType::UniformBuffer, "OpenGL Uniform Buffer");

		// Register with GPU Resource Inspector
		GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_UNIFORM_BUFFER, "UniformBuffer");
	}
	
	OpenGLUniformBuffer::OpenGLUniformBuffer(const u32 size, const u32 binding, const GLenum usage)
	{
		glCreateBuffers(1, &m_RendererID);
		glNamedBufferStorage(m_RendererID, size, nullptr, usage);
		glBindBufferRange(GL_UNIFORM_BUFFER, binding, m_RendererID, 0, size);
		RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);
		
		// Track GPU memory allocation
		OLO_TRACK_GPU_ALLOC(this, size, RendererMemoryTracker::ResourceType::UniformBuffer, "OpenGL Uniform Buffer (storage)");

		// Register with GPU Resource Inspector
		GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_UNIFORM_BUFFER, "UniformBuffer (storage)");
	}
	
	OpenGLUniformBuffer::~OpenGLUniformBuffer()
	{
		// Track GPU memory deallocation
		OLO_TRACK_DEALLOC(this);
		
		// Unregister from GPU Resource Inspector
		GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);
		
		glDeleteBuffers(1, &m_RendererID);
	}

	void OpenGLUniformBuffer::SetData(const UniformData& data)
	{		
		glNamedBufferSubData(m_RendererID, data.offset, data.size, data.data);
	}
}
