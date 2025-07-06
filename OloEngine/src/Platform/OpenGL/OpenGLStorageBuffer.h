#pragma once

#include "OloEngine/Renderer/StorageBuffer.h"
#include <glad/gl.h>

namespace OloEngine
{
    class OpenGLStorageBuffer : public StorageBuffer
    {
    public:
        OpenGLStorageBuffer(u32 size, const void* data = nullptr, BufferUsage usage = BufferUsage::Dynamic);
        ~OpenGLStorageBuffer() override;

        void SetData(const void* data, u32 size, u32 offset = 0) override;
        void GetData(void* data, u32 size, u32 offset = 0) override;
        
        void Bind(u32 bindingPoint) override;
        void Unbind() override;
        
        // Phase 6.1: Resource handle caching support
        u32 GetRendererID() const override { return m_RendererID; }
        
    private:
        u32 m_RendererID = 0;
        GLenum GetOpenGLUsage(BufferUsage usage) const;
    };
}
