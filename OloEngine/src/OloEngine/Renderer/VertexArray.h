#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Core/Ref.h"

#include <memory>

namespace OloEngine
{
    class VertexArray : public RefCounted
    {
      public:
        virtual ~VertexArray() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) = 0;
        virtual void AddInstanceBuffer(const Ref<VertexBuffer>& vertexBuffer) = 0;
        virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) = 0;

        [[nodiscard("Store this!")]] virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const = 0;
        [[nodiscard("Store this!")]] virtual const Ref<IndexBuffer>& GetIndexBuffer() const = 0;

        static Ref<VertexArray> Create();

        [[nodiscard]] virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691 Phase 2 step 3). Sibling of GetRendererID() during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
    };
} // namespace OloEngine
