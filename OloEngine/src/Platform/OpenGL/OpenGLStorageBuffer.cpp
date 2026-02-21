#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLStorageBuffer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glad/gl.h>

namespace OloEngine
{
    OpenGLStorageBuffer::OpenGLStorageBuffer(u32 size, u32 binding)
        : m_Size(size), m_Binding(binding)
    {
        glCreateBuffers(1, &m_RendererID);
        glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, m_RendererID);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);

        OLO_TRACK_GPU_ALLOC(this, size, RendererMemoryTracker::ResourceType::StorageBuffer, "OpenGL Storage Buffer");
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_SHADER_STORAGE_BUFFER, "StorageBuffer");
    }

    OpenGLStorageBuffer::~OpenGLStorageBuffer()
    {
        OLO_TRACK_DEALLOC(this);
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);
        glDeleteBuffers(1, &m_RendererID);
    }

    void OpenGLStorageBuffer::Bind() const
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, m_Binding, m_RendererID);
    }

    void OpenGLStorageBuffer::Unbind() const
    {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, m_Binding, 0);
    }

    void OpenGLStorageBuffer::SetData(const void* data, u32 size, u32 offset)
    {
        OLO_CORE_ASSERT(offset + size <= m_Size, "StorageBuffer::SetData out of range!");
        glNamedBufferSubData(m_RendererID, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), data);
    }

    void OpenGLStorageBuffer::GetData(void* outData, u32 size, u32 offset) const
    {
        OLO_CORE_ASSERT(offset + size <= m_Size, "StorageBuffer::GetData out of range!");
        glGetNamedBufferSubData(m_RendererID, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(size), outData);
    }

    void OpenGLStorageBuffer::Resize(u32 newSize)
    {
        OLO_TRACK_DEALLOC(this);
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);
        glDeleteBuffers(1, &m_RendererID);

        m_Size = newSize;

        glCreateBuffers(1, &m_RendererID);
        glNamedBufferData(m_RendererID, newSize, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, m_Binding, m_RendererID);

        OLO_TRACK_GPU_ALLOC(this, newSize, RendererMemoryTracker::ResourceType::StorageBuffer, "OpenGL Storage Buffer");
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_SHADER_STORAGE_BUFFER, "StorageBuffer");
    }
} // namespace OloEngine
