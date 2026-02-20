#pragma once

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

      private:
        u32 m_RendererID{};
        u32 m_VertexBufferIndex = 0;
        std::vector<Ref<VertexBuffer>> m_VertexBuffers;
        Ref<IndexBuffer> m_IndexBuffer;
    };
} // namespace OloEngine
