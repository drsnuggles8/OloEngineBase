#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Base.h"

#include <cstring>

namespace OloEngine
{
    // @brief Usage hint for StorageBuffer allocation.
    enum class StorageBufferUsage : u8
    {
        DynamicDraw, // CPU writes, GPU reads (default)
        DynamicCopy, // GPU writes, GPU reads (compute output)
    };

    // @brief Shader Storage Buffer Object (SSBO) abstraction.
    //
    // SSBOs are larger than UBOs and support both read and write access from shaders.
    // They are bound to a specific binding point and can be used for general-purpose
    // GPU data storage (particle systems, compute results, indirect draw buffers, etc.).
    class StorageBuffer : public RefCounted
    {
      public:
        virtual ~StorageBuffer() = default;

        // Bind the buffer to its binding point
        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        // Upload data to the GPU
        virtual void SetData(const void* data, u32 size, u32 offset = 0) = 0;

        // Read data back from the GPU (requires GPU-to-CPU sync)
        virtual void GetData(void* outData, u32 size, u32 offset = 0) const = 0;

        // Typed convenience wrapper for reading structured data from the GPU
        template<typename T>
        T GetData(u32 offset = 0) const
        {
            OLO_CORE_ASSERT(offset + sizeof(T) <= GetSize(), "StorageBuffer::GetData<T> out of range!");
            T result;
            GetData(&result, static_cast<u32>(sizeof(T)), offset);
            return result;
        }

        // Resize the buffer (invalidates existing data)
        virtual void Resize(u32 newSize) = 0;

        [[nodiscard]] virtual u32 GetRendererID() const = 0;
        [[nodiscard]] virtual u32 GetSize() const = 0;
        [[nodiscard]] virtual u32 GetBinding() const = 0;

        static Ref<StorageBuffer> Create(u32 size, u32 binding, StorageBufferUsage usage = StorageBufferUsage::DynamicDraw);
    };
} // namespace OloEngine
