#pragma once

#include "OloEngine/Renderer/IndexBuffer.h"

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

      private:
        u32 m_RendererID{};
        u32 m_Count;
    };
} // namespace OloEngine
