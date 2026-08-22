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

        // Adds a buffer whose declared elements feed CONSTANT values: every
        // vertex of every instance fetches from offset 0 (GL: a stride-0
        // binding), so an 8-byte buffer can back a vec2 attribute for any
        // vertex count. Exists to keep attribute layouts identical across
        // meshes that lack an optional stream — a program that reads an
        // attribute which is buffer-backed on one mesh and disabled on the
        // next makes NVIDIA specialize a vertex-shader variant per layout
        // permutation (GL debug id 131218, "recompiled based on GL state").
        // Vulkan implements this as a no-op: the vertex-pull path stubs
        // optional streams in-shader and has no layout-specialization issue.
        virtual void AddConstantVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) = 0;

        virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) = 0;

        [[nodiscard("Store this!")]] virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const = 0;
        [[nodiscard("Store this!")]] virtual const Ref<IndexBuffer>& GetIndexBuffer() const = 0;

        static Ref<VertexArray> Create();

        [[nodiscard]] virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691). Sibling of GetRendererID during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
    };
} // namespace OloEngine
