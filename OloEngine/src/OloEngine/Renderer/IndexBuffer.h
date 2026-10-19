#pragma once

#include <utility>
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

namespace OloEngine
{
    // Currently OloEngine only supports 32-bit index buffers
    class IndexBuffer : public RefCounted
    {
      public:
        virtual ~IndexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        [[nodiscard("Store this!")]] virtual u32 GetCount() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetBufferHandle() const = 0;
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
        // Zero when the active backend does not expose buffer device addresses.
        [[nodiscard]] virtual u64 GetDeviceAddress() const = 0;

        static Ref<IndexBuffer> Create(u32* indices, u32 size);
        // Immutable storage on GL, for the same reason as
        // VertexBuffer::CreateImmutable: no resident system-memory copy (#1608).
        static Ref<IndexBuffer> CreateImmutable(const u32* indices, u32 count);
    };
} // namespace OloEngine
