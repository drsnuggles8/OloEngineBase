#pragma once

#include "OloEngine/Renderer/StorageBuffer.h"
#include <glad/gl.h>

namespace OloEngine
{
    class OpenGLStorageBuffer : public StorageBuffer
    {
      public:
        OpenGLStorageBuffer(u32 size, u32 binding);
        ~OpenGLStorageBuffer() override;

        void Bind() const override;
        void Unbind() const override;

        void SetData(const void* data, u32 size, u32 offset = 0) override;
        void GetData(void* outData, u32 size, u32 offset = 0) const override;
        void Resize(u32 newSize) override;

        [[nodiscard]] u32 GetRendererID() const override { return m_RendererID; }
        [[nodiscard]] u32 GetSize() const override { return m_Size; }
        [[nodiscard]] u32 GetBinding() const override { return m_Binding; }

      private:
        u32 m_RendererID = 0;
        u32 m_Size = 0;
        u32 m_Binding = 0;
    };
} // namespace OloEngine
