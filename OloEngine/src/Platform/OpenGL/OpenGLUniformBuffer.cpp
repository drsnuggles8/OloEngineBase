#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLUniformBuffer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glad/gl.h>

namespace OloEngine
{

    OpenGLUniformBuffer::OpenGLUniformBuffer(const u32 size, const u32 binding)
        : m_Binding(binding), m_AllocatedSize(size)
    {
        OLO_PROFILE_FUNCTION();

        glCreateBuffers(1, &m_RendererID);
        glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, binding, m_RendererID);
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this, size, RendererMemoryTracker::ResourceType::UniformBuffer, "OpenGL Uniform Buffer");

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_UNIFORM_BUFFER, "UniformBuffer");
    }

    OpenGLUniformBuffer::OpenGLUniformBuffer(const u32 size, const u32 binding, const GLbitfield flags)
        : m_Binding(binding), m_AllocatedSize(size)
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_ASSERT(flags & GL_DYNAMIC_STORAGE_BIT,
                        "OpenGLUniformBuffer storage flags must include GL_DYNAMIC_STORAGE_BIT for SetData() to work");
        glCreateBuffers(1, &m_RendererID);
        glNamedBufferStorage(m_RendererID, size, nullptr, flags);
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
        OLO_CORE_ASSERT(
            data.offset <= m_AllocatedSize && data.size <= m_AllocatedSize - data.offset,
            "UBO SetData overflow: offset({}) + size({}) > allocated({}), binding={}, GL id={}",
            data.offset, data.size, m_AllocatedSize, m_Binding, m_RendererID);
        glNamedBufferSubData(m_RendererID, data.offset, data.size, data.data);
    }

    void OpenGLUniformBuffer::Bind() const
    {
        glBindBufferBase(GL_UNIFORM_BUFFER, m_Binding, m_RendererID);
    }
} // namespace OloEngine
