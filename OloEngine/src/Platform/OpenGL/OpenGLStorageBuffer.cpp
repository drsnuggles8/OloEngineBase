#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLStorageBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glad/gl.h>

namespace OloEngine
{
    OpenGLStorageBuffer::OpenGLStorageBuffer(u32 size, u32 binding, StorageBufferUsage usage)
        : m_Size(size), m_Binding(binding), m_Usage(usage)
    {
        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferData(m_RendererID, size, nullptr, ToGLUsage());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, m_RendererID);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);

        OLO_TRACK_GPU_ALLOC(this, size, RendererMemoryTracker::ResourceType::StorageBuffer, "OpenGL Storage Buffer");
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_SHADER_STORAGE_BUFFER, "StorageBuffer");
    }

    OpenGLStorageBuffer::~OpenGLStorageBuffer()
    {
        OLO_TRACK_DEALLOC(this);
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);

        u32 id = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([id]()
                                                      { glDeleteBuffers(1, &id); });
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

    void OpenGLStorageBuffer::ClearData()
    {
        glClearNamedBufferData(m_RendererID, GL_R8, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    void OpenGLStorageBuffer::ClearData(u32 offset, u32 size)
    {
        OLO_CORE_ASSERT(static_cast<u64>(offset) + size <= m_Size, "StorageBuffer::ClearData range out of bounds!");
        OLO_CORE_ASSERT((offset % 4u) == 0u && (size % 4u) == 0u, "StorageBuffer::ClearData range must be 4-byte aligned");
        if (size == 0u)
            return;
        glClearNamedBufferSubData(m_RendererID, GL_R8, offset, size, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    }

    void OpenGLStorageBuffer::Resize(u32 newSize)
    {
        OLO_PROFILE_FUNCTION();

        OLO_TRACK_DEALLOC(this);
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);

        u32 oldId = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([oldId]()
                                                      { glDeleteBuffers(1, &oldId); });

        m_Size = newSize;

        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferData(m_RendererID, newSize, nullptr, ToGLUsage());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, m_Binding, m_RendererID);

        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::BufferBinds, 1);
        OLO_TRACK_GPU_ALLOC(this, newSize, RendererMemoryTracker::ResourceType::StorageBuffer, "OpenGL Storage Buffer");
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_SHADER_STORAGE_BUFFER, "StorageBuffer");
    }

    GLenum OpenGLStorageBuffer::ToGLUsage() const
    {
        switch (m_Usage)
        {
            case StorageBufferUsage::DynamicDraw:
                return GL_DYNAMIC_DRAW;
            case StorageBufferUsage::DynamicCopy:
                return GL_DYNAMIC_COPY;
            default:
                OLO_CORE_ASSERT(false, "Unknown StorageBufferUsage!");
                return GL_DYNAMIC_DRAW;
        }
    }
} // namespace OloEngine
