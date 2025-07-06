#include "OloEnginePCH.h"
#include "OpenGLStorageBuffer.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"

namespace OloEngine
{
    OpenGLStorageBuffer::OpenGLStorageBuffer(u32 size, const void* data, BufferUsage usage)
    {
        OLO_PROFILE_FUNCTION();
        
        m_Size = size;
        m_IsReadWrite = true; // SSBOs support read/write by default
        
        // Generate buffer
        glGenBuffers(1, &m_RendererID);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_RendererID);
        
        // Allocate storage
        GLenum glUsage = GetOpenGLUsage(usage);
        glBufferData(GL_SHADER_STORAGE_BUFFER, size, data, glUsage);
        
        // Update local cache if data was provided
        if (data)
        {
            UpdateLocalData(data, size);
        }
        
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this, 
                           size, 
                           RendererMemoryTracker::ResourceType::StorageBuffer, 
                           "OpenGL Storage Buffer");
        
        OLO_CORE_TRACE("OpenGLStorageBuffer created: ID={0}, Size={1} bytes", m_RendererID, size);
    }

    OpenGLStorageBuffer::~OpenGLStorageBuffer()
    {
        OLO_PROFILE_FUNCTION();
        
        // Track GPU memory deallocation
        OLO_TRACK_DEALLOC(this);
        
        glDeleteBuffers(1, &m_RendererID);
        
        OLO_CORE_TRACE("OpenGLStorageBuffer destroyed: ID={0}", m_RendererID);
    }

    void OpenGLStorageBuffer::SetData(const void* data, u32 size, u32 offset)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!data || size == 0)
        {
            OLO_CORE_WARN("Attempting to set null or zero-size data to StorageBuffer");
            return;
        }
        
        if (offset + size > m_Size)
        {
            OLO_CORE_ERROR("Data size ({0}) + offset ({1}) exceeds buffer size ({2})", size, offset, m_Size);
            return;
        }
        
        // Update GPU data
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_RendererID);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, offset, size, data);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        
        // Update local cache
        UpdateLocalData(data, size, offset);
        
        OLO_CORE_TRACE("StorageBuffer data updated: ID={0}, Size={1}, Offset={2}", m_RendererID, size, offset);
    }

    void OpenGLStorageBuffer::GetData(void* data, u32 size, u32 offset)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!data || size == 0)
        {
            OLO_CORE_WARN("Attempting to get data with null pointer or zero size from StorageBuffer");
            return;
        }
        
        if (offset + size > m_Size)
        {
            OLO_CORE_ERROR("Data size ({0}) + offset ({1}) exceeds buffer size ({2})", size, offset, m_Size);
            return;
        }
        
        // Try to read from local cache first (faster)
        if (m_LocalData && offset + size <= m_Size)
        {
            std::memcpy(data, static_cast<const u8*>(m_LocalData) + offset, size);
            OLO_CORE_TRACE("StorageBuffer data read from cache: ID={0}, Size={1}, Offset={2}", m_RendererID, size, offset);
            return;
        }
        
        // Fallback to GPU read (slower)
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_RendererID);
        
        // Map the buffer for reading
        void* mappedData = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, offset, size, GL_MAP_READ_BIT);
        if (mappedData)
        {
            std::memcpy(data, mappedData, size);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            OLO_CORE_TRACE("StorageBuffer data read from GPU: ID={0}, Size={1}, Offset={2}", m_RendererID, size, offset);
        }
        else
        {
            OLO_CORE_ERROR("Failed to map StorageBuffer for reading: ID={0}", m_RendererID);
        }
        
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void OpenGLStorageBuffer::Bind(u32 bindingPoint)
    {
        OLO_PROFILE_FUNCTION();
        
        m_BindingPoint = bindingPoint;
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, bindingPoint, m_RendererID);
        
        OLO_CORE_TRACE("StorageBuffer bound: ID={0}, Binding={1}", m_RendererID, bindingPoint);
    }

    void OpenGLStorageBuffer::Unbind()
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_BindingPoint != 0)
        {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, m_BindingPoint, 0);
            OLO_CORE_TRACE("StorageBuffer unbound: ID={0}, Binding={1}", m_RendererID, m_BindingPoint);
            m_BindingPoint = 0;
        }
    }

    GLenum OpenGLStorageBuffer::GetOpenGLUsage(BufferUsage usage) const
    {
        switch (usage)
        {
            case BufferUsage::Static:    return GL_STATIC_DRAW;
            case BufferUsage::Dynamic:   return GL_DYNAMIC_DRAW;
            case BufferUsage::Stream:    return GL_STREAM_DRAW;
            default:
                OLO_CORE_WARN("Unknown BufferUsage, defaulting to GL_DYNAMIC_DRAW");
                return GL_DYNAMIC_DRAW;
        }
    }
}
