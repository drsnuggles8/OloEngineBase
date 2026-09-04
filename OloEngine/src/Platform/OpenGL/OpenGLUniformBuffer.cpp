#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLUniformBuffer.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
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
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
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
        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, m_RendererID, RHI::Backend::OpenGL);
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

        u32 id = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([id]()
                                                      { glDeleteBuffers(1, &id); });
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

    void OpenGLUniformBuffer::Unbind() const
    {
        // ONLY CLEAR THE SLOT IF WE ARE STILL ITS OCCUPANT. A UniformBuffer
        // claims its binding point at construction and nothing rebinds
        // afterwards, so a buffer created later on the same point has already
        // displaced this one -- and an unconditional clear here would evict the
        // buffer that legitimately owns the slot now, leaving the next draw
        // reading zeroes. That is the same last-created-wins hazard
        // notes-renderer.md describes, seen from the other side.
        //
        // The Vulkan implementation has the identical guard against its own
        // binding-state record; this one was missed on the first pass and is
        // pinned by UniformBufferBindingOwnershipTest.
        GLint current = 0;
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, m_Binding, &current);
        if (static_cast<u32>(current) == m_RendererID)
            glBindBufferBase(GL_UNIFORM_BUFFER, m_Binding, 0);
    }
} // namespace OloEngine
