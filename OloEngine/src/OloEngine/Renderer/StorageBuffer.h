#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Base.h"

#include <cstring>
#include <type_traits>

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
            static_assert(std::is_trivially_copyable_v<T>, "StorageBuffer::GetData<T> requires a trivially copyable type");
            OLO_CORE_ASSERT(offset + sizeof(T) <= GetSize(), "StorageBuffer::GetData<T> out of range!");
            T result;
            GetData(&result, static_cast<u32>(sizeof(T)), offset);
            return result;
        }

        // Zero-fill the entire buffer on the GPU (no CPU allocation)
        virtual void ClearData() = 0;

        // Zero-fill [offset, offset + size) on the GPU. For a buffer that GPU
        // kernels write into every frame and that carries a per-frame header
        // in front of persistent records (DDGI's probe-aux block, #1015):
        // a SetData of zeros would be a CPU write into a live buffer, which on
        // the Vulkan backend installs a frame snapshot as the root address for
        // every later draw (VulkanStorageBuffer.h, rule 6) -- a clear stays in
        // the GPU command stream on both backends. Offset and size are
        // multiples of 4 (vkCmdFillBuffer's granularity).
        virtual void ClearData(u32 offset, u32 size) = 0;

        // Resize the buffer (invalidates existing data)
        virtual void Resize(u32 newSize) = 0;

        [[nodiscard]] virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691). Sibling of GetRendererID during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
        [[nodiscard]] virtual u32 GetSize() const = 0;
        [[nodiscard]] virtual u32 GetBinding() const = 0;

        // The PERSISTENT buffer's device address, for a shader that reaches
        // this buffer through GL_EXT_buffer_reference rather than a binding
        // slot (issue #1055's emissive-triangle table is the first consumer).
        // Zero when the active backend does not expose buffer device addresses
        // (OpenGL) — the same contract VertexBuffer / IndexBuffer::GetDeviceAddress
        // carry. Resize mints a new one, so resolve it every frame after the
        // last SetData rather than caching it.
        [[nodiscard]] virtual u64 GetDeviceAddress() const
        {
            return 0;
        }

        // A binding number meaning "publish at no slot". For a buffer that is
        // only ever reached by device address (GL_EXT_buffer_reference): both
        // backends skip the construction-time and Bind()-time publication, so
        // the buffer never touches the shared indexed-binding state. The
        // portable SSBO namespace is full (ShaderBindingLayout.h); this is how
        // a by-address buffer stays out of it rather than squatting on a slot.
        static constexpr u32 kNoBinding = 0xFFFFFFFFu;

        static Ref<StorageBuffer> Create(u32 size, u32 binding, StorageBufferUsage usage = StorageBufferUsage::DynamicDraw);
    };
} // namespace OloEngine
