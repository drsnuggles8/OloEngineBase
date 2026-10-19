#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
    class OpenGLVertexBuffer : public VertexBuffer
    {
      public:
        explicit OpenGLVertexBuffer(const u32 size);
        OpenGLVertexBuffer(const u32 size, const GLenum usage);
        OpenGLVertexBuffer(const f32* vertices, u32 size);
        OpenGLVertexBuffer(const f32* vertices, u32 size, GLenum usage);
        OpenGLVertexBuffer(const void* data, u32 size);
        OpenGLVertexBuffer(const void* data, u32 size, GLenum usage);
        ~OpenGLVertexBuffer() override;

        void Bind() const override;
        void Unbind() const override;

        void SetData(const VertexData& data) override;

        // ImmutableVertexBufferUpload's two GL steps (#1608), each checked: the
        // destination's storage really is `m_Size` bytes (an out-of-memory
        // allocation leaves it empty with nothing else to say so), and a chunk
        // written to `staging` and copied to `offset` here raised no GL error.
        [[nodiscard]] bool HasItsStorage() const;
        [[nodiscard]] bool UploadChunkThrough(OpenGLVertexBuffer& staging, u32 offset, const void* data, u32 bytes);

        [[nodiscard("Store this!")]] const BufferLayout& GetLayout() const override
        {
            return m_Layout;
        }
        void SetLayout(const BufferLayout& layout) override
        {
            m_Layout = layout;
        }
        [[nodiscard("Store this!")]] u32 GetBufferHandle() const override
        {
            return m_RendererID;
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] u64 GetDeviceAddress() const override
        {
            return 0;
        }

      private:
        u32 m_RendererID{};
        u32 m_Size{}; // Buffer size in bytes for memory tracking
        // False for immutable storage without GL_DYNAMIC_STORAGE_BIT: SetData cannot write it.
        bool m_Refillable = true;
        BufferLayout m_Layout;
        RHI::ScopedResourceHandle m_RHIHandle;
    };
} // namespace OloEngine
