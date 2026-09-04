#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glad/gl.h>

namespace OloEngine
{
    class OpenGLUniformBuffer : public UniformBuffer
    {
      public:
        OpenGLUniformBuffer(const u32 size, const u32 binding);
        OpenGLUniformBuffer(const u32 size, const u32 binding, const GLbitfield flags);
        ~OpenGLUniformBuffer() override;

        void SetData(const UniformData& data) override;
        void Bind() const override;
        void Unbind() const override;

        // Resource handle caching support
        u32 GetRendererID() const override
        {
            return m_RendererID;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }

        u32 GetAllocatedSize() const
        {
            return m_AllocatedSize;
        }

      private:
        u32 m_RendererID = 0;
        // Generation-checked identity for m_RendererID above, kept in
        // lockstep by m_RHIHandle.Sync() at every site that assigns the
        // native name. RAII retires the entry, so a handle to a destroyed
        // object can never resolve to a recycled GL name (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_Binding = 0;
        u32 m_AllocatedSize = 0;
    };
} // namespace OloEngine
