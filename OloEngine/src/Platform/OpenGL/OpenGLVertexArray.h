#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    class OpenGLVertexArray : public VertexArray
    {
      public:
        OpenGLVertexArray();
        ~OpenGLVertexArray() override;

        void Bind() const override;
        void Unbind() const override;

        void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
        void AddInstanceBuffer(const Ref<VertexBuffer>& vertexBuffer) override;
        void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) override;

        [[nodiscard("Store this!")]] const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const override
        {
            return m_VertexBuffers;
        }
        [[nodiscard("Store this!")]] const Ref<IndexBuffer>& GetIndexBuffer() const override
        {
            return m_IndexBuffer;
        }

        [[nodiscard]] u32 GetRendererID() const override
        {
            return m_RendererID;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }

      private:
        u32 m_RendererID{};
        // Generation-checked identity for m_RendererID above, kept in
        // lockstep by m_RHIHandle.Sync() at every site that assigns the
        // native name. RAII retires the entry, so a handle to a destroyed
        // object can never resolve to a recycled GL name (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_VertexBufferIndex = 0;
        std::vector<Ref<VertexBuffer>> m_VertexBuffers;
        Ref<IndexBuffer> m_IndexBuffer;
    };
} // namespace OloEngine
