#pragma once

#include "OloEngine/Renderer/UniformBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
    class OpenGLUniformBuffer : public UniformBuffer
    {
      public:
        OpenGLUniformBuffer(const u32 size, const u32 binding);
        OpenGLUniformBuffer(const u32 size, const u32 binding, const GLenum usage);
        ~OpenGLUniformBuffer() override;

        void SetData(const UniformData& data) override;
        void Bind() const override;

        // Phase 6.1: Resource handle caching support
        u32 GetRendererID() const override
        {
            return m_RendererID;
        }

        u32 GetAllocatedSize() const { return m_AllocatedSize; }

      private:
        u32 m_RendererID = 0;
        u32 m_Binding = 0;
        u32 m_AllocatedSize = 0;
    };
} // namespace OloEngine
