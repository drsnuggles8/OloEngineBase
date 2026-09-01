#pragma once

#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"

#include <glad/gl.h>

namespace OloEngine
{
    class OpenGLIndexBuffer : public IndexBuffer
    {
      public:
        OpenGLIndexBuffer(u32 const* indices, u32 count);
        OpenGLIndexBuffer(u32 const* indices, u32 count, GLenum usage);
        ~OpenGLIndexBuffer() override;

        void Bind() const override;
        void Unbind() const override;

        [[nodiscard("Store this!")]] u32 GetCount() const override
        {
            return m_Count;
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
        u32 m_Count;
        RHI::ScopedResourceHandle m_RHIHandle;
    };
} // namespace OloEngine
