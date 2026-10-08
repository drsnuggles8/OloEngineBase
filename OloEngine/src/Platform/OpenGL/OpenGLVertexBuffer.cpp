#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLVertexBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#include <glad/gl.h>

namespace OloEngine
{
    OpenGLVertexBuffer::OpenGLVertexBuffer(const u32 size) : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();

        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferData(m_RendererID, size, nullptr, GL_DYNAMIC_DRAW);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            size,
                            RendererMemoryTracker::ResourceType::VertexBuffer,
                            "OpenGL VertexBuffer (dynamic)");
        RendererMemory::BindBackingResourceHandle(this, RHI::HashKey(m_RHIHandle.Get()));

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (dynamic)");
    }
    OpenGLVertexBuffer::OpenGLVertexBuffer(const u32 size, const GLenum usage) : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();

        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferStorage(m_RendererID, size, nullptr, usage);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            size,
                            RendererMemoryTracker::ResourceType::VertexBuffer,
                            "OpenGL VertexBuffer (storage)");
        RendererMemory::BindBackingResourceHandle(this, RHI::HashKey(m_RHIHandle.Get()));

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (storage)");
    }
    OpenGLVertexBuffer::OpenGLVertexBuffer(const f32* const vertices, const u32 size) : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();

        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferData(m_RendererID, size, vertices, GL_STATIC_DRAW);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            size,
                            RendererMemoryTracker::ResourceType::VertexBuffer,
                            "OpenGL VertexBuffer (static)");
        RendererMemory::BindBackingResourceHandle(this, RHI::HashKey(m_RHIHandle.Get()));

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (static)");
    }
    OpenGLVertexBuffer::OpenGLVertexBuffer(const f32* vertices, const u32 size, const GLenum usage) : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();

        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferStorage(m_RendererID, size, vertices, usage);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            size,
                            RendererMemoryTracker::ResourceType::VertexBuffer,
                            "OpenGL VertexBuffer (static storage)");
        RendererMemory::BindBackingResourceHandle(this, RHI::HashKey(m_RHIHandle.Get()));

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (static storage)");
    }

    OpenGLVertexBuffer::OpenGLVertexBuffer(const void* data, const u32 size) : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();

        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferData(m_RendererID, size, data, GL_STATIC_DRAW);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            size,
                            RendererMemoryTracker::ResourceType::VertexBuffer,
                            "OpenGL VertexBuffer (static, raw)");
        RendererMemory::BindBackingResourceHandle(this, RHI::HashKey(m_RHIHandle.Get()));

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (static, raw)");
    }

    OpenGLVertexBuffer::OpenGLVertexBuffer(const void* data, const u32 size, const GLenum usage) : m_Size(size)
    {
        OLO_PROFILE_FUNCTION();

        glCreateBuffers(1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
        glNamedBufferStorage(m_RendererID, size, data, usage);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            size,
                            RendererMemoryTracker::ResourceType::VertexBuffer,
                            "OpenGL VertexBuffer (raw storage)");
        RendererMemory::BindBackingResourceHandle(this, RHI::HashKey(m_RHIHandle.Get()));

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterBuffer(m_RendererID, GL_ARRAY_BUFFER, "VertexBuffer (raw storage)");
    }

    OpenGLVertexBuffer::~OpenGLVertexBuffer()
    {
        OLO_PROFILE_FUNCTION();
        // The GL object is deleted two frames from now; until then its storage is still
        // resident and the memory report counts it as retiring (#1342).
        const u64 retireTicket = OLO_TRACK_RETIRE(this);

        // Unregister from GPU Resource Inspector
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);
        m_RHIHandle.Reset();

        u32 id = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([id, retireTicket]()
                                                      {
                                                          glDeleteBuffers(1, &id);
                                                          OLO_TRACK_RELEASE_RETIRED(retireTicket); });
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

        OLO_CORE_ASSERT(
            data.size <= m_Size,
            "VertexBuffer SetData overflow: data.size({}) > allocated({}), GL id={}",
            data.size, m_Size, m_RendererID);
        if (data.size > m_Size)
            return;
        glNamedBufferSubData(m_RendererID, 0, data.size, data.data);
    }
} // namespace OloEngine
