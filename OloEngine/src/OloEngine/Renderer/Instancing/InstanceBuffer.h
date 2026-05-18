#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <span>

namespace OloEngine
{
    // @brief Typed wrapper around an SSBO of InstanceData laid out for std430.
    //
    // Owns a single StorageBuffer bound at ShaderBindingLayout::SSBO_INSTANCE_DATA.
    // Capacity grows by doubling on overflow; the GPU buffer is reallocated through
    // StorageBuffer::Resize() (frame-deferred deletion handled by the platform layer).
    //
    // Lifetime model: a single InstanceBuffer is reused across frames. Callers
    // call Upload(span) at most once per frame per use site. The buffer's binding
    // is sticky after Bind(); no rebind is needed unless another SSBO has taken
    // over slot SSBO_INSTANCE_DATA in the interim.
    class InstanceBuffer : public RefCounted
    {
      public:
        // Initial capacity expressed in instance count (NOT bytes).
        explicit InstanceBuffer(u32 initialCapacity = 64);

        // Upload `instances.size()` records to the GPU starting at element 0.
        // Grows the underlying SSBO (by doubling, then to the exact requested
        // size if doubling is still insufficient) when capacity is exceeded.
        // The recorded instance count is updated to instances.size().
        void Upload(std::span<const InstanceData> instances);

        // Force the buffer to hold at least `requiredCount` instances of
        // capacity. Does not change the recorded instance count. Use this
        // ahead of multiple partial UploadRange() calls.
        void EnsureCapacity(u32 requiredCount);

        // Upload a sub-range without changing the recorded instance count.
        // `offset` and `data.size()` must fit within the current capacity —
        // call EnsureCapacity() first if not.
        void UploadRange(u32 offset, std::span<const InstanceData> data);

        // Bind to ShaderBindingLayout::SSBO_INSTANCE_DATA. Safe to call every
        // frame; OpenGL handles redundant binds cheaply.
        void Bind() const;

        // Number of instances currently considered "live" (set by Upload()).
        [[nodiscard]] u32 GetCount() const
        {
            return m_Count;
        }

        // Allocated capacity in instances.
        [[nodiscard]] u32 GetCapacity() const
        {
            return m_Capacity;
        }

        // Underlying SSBO byte size (= capacity * sizeof(InstanceData)).
        [[nodiscard]] u32 GetSizeBytes() const;

        // Access the underlying storage (for render-pass code that needs to
        // pass the SSBO handle into a CommandBucket entry, etc.).
        [[nodiscard]] const Ref<StorageBuffer>& GetStorage() const
        {
            return m_Storage;
        }

      private:
        Ref<StorageBuffer> m_Storage;
        u32 m_Capacity = 0; // in instances
        u32 m_Count = 0;    // in instances
    };
} // namespace OloEngine
