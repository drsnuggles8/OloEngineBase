#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include <glad/gl.h>

namespace OloEngine
{
    class OpenGLStorageBuffer : public StorageBuffer
    {
      public:
        OpenGLStorageBuffer(u32 size, u32 binding, StorageBufferUsage usage = StorageBufferUsage::DynamicDraw);
        ~OpenGLStorageBuffer() override;

        void Bind() const override;
        void Unbind() const override;

        void SetData(const void* data, u32 size, u32 offset = 0) override;
        void GetData(void* outData, u32 size, u32 offset = 0) const override;
        void ClearData() override;
        void ClearData(u32 offset, u32 size) override;
        void Resize(u32 newSize) override;

        [[nodiscard]] u32 GetRendererID() const override
        {
            return m_RendererID;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] u32 GetSize() const override
        {
            return m_Size;
        }
        [[nodiscard]] u32 GetBinding() const override
        {
            return m_Binding;
        }

      private:
        [[nodiscard]] GLenum ToGLUsage() const;

        u32 m_RendererID = 0;
        // Generation-checked identity for m_RendererID above, kept in
        // lockstep by m_RHIHandle.Sync() at every site that assigns the
        // native name. RAII retires the entry, so a handle to a destroyed
        // object can never resolve to a recycled GL name (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_Size = 0;
        u32 m_Binding = 0;
        StorageBufferUsage m_Usage = StorageBufferUsage::DynamicDraw;
    };
} // namespace OloEngine
