#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    // TODO(olbu): Add Create() functions for the new constructors of OpenGLUniformBuffer
    class UniformBuffer : public RefCounted
    {
      protected:
        // CPU-side cache of the buffer data to allow reading
        void* m_LocalData = nullptr;
        u32 m_Size = 0;

      public:
        virtual ~UniformBuffer()
        {
            if (m_LocalData)
                delete[] static_cast<u8*>(m_LocalData);
        }

        // Original method using UniformData struct
        virtual void SetData(const UniformData& data) = 0;

        // Re-bind this buffer to its original binding point
        virtual void Bind() const = 0;

        // Freeze backend upload caches before sharing this read-only object
        // across recording items. Call on the primary after the last SetData;
        // the captured buffer need not occupy a currently seeded binding slot.
        virtual void PrepareForParallelRead() {}

        // Releases this buffer's binding point. `StorageBuffer` has always had
        // this; `UniformBuffer` did not, which left the only way to clear a UBO
        // slot a raw `glBindBufferBase(..., 0)` in engine-layer code -- exactly
        // what the RHI boundary ratchet forbids (issue #691, ADR 0011).
        //
        // It matters because a UniformBuffer claims its binding point at
        // CONSTRUCTION and nothing rebinds afterwards, so a destroyed buffer
        // left on a shared slot is handed to whatever runs next. See
        // notes-renderer.md.
        virtual void Unbind() const = 0;

        // Resource handle caching support
        virtual u32 GetRendererID() const = 0;

        // Generation-checked identity, minted by RHI::ResourceRegistry
        // (issue #691). Sibling of GetRendererID during the
        // migration: that one hands out the raw backend name and is deleted once
        // every caller has moved. Turning a handle back into a native object is
        // Platform/<Backend>/'s business.
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
        virtual u32 GetSize() const
        {
            return m_Size;
        }

        // Read only while the source object has no writer (e.g. before a
        // recording fork). Used to seed an item-owned upload object.
        [[nodiscard]] std::span<const u8> GetCachedData() const
        {
            return m_LocalData ? std::span<const u8>(static_cast<const u8*>(m_LocalData), m_Size)
                               : std::span<const u8>{};
        }

        // New convenience method to set data directly
        virtual void SetData(const void* data, u32 size, u32 offset = 0)
        {
            // Grow the CPU-side buffer if needed
            if (u32 requiredSize = offset + size; requiredSize > m_Size)
            {
                auto* newBuf = new u8[requiredSize]{};
                if (m_LocalData)
                {
                    std::memcpy(newBuf, m_LocalData, m_Size);
                    delete[] static_cast<u8*>(m_LocalData);
                }
                m_LocalData = newBuf;
                m_Size = requiredSize;
            }

            if (m_LocalData && size > 0)
            {
                std::memcpy(static_cast<u8*>(m_LocalData) + offset, data, size);
            }

            // Send to GPU
            UniformData uniformData;
            uniformData.data = data;
            uniformData.size = size;
            uniformData.offset = offset;
            SetData(uniformData);
        }

        // New method to read data back from the CPU-side cache
        template<typename T>
        T GetData() const
        {
            static_assert(std::is_trivially_copyable_v<T>, "UniformBuffer::GetData<T> requires a trivially copyable type");
            OLO_CORE_ASSERT(m_LocalData != nullptr, "Cannot read from uninitialized UBO data!");
            OLO_CORE_ASSERT(sizeof(T) <= m_Size, "Type size exceeds UBO size!");

            T result;
            std::memcpy(&result, m_LocalData, sizeof(T));
            return result;
        }

        static Ref<UniformBuffer> Create(u32 size, u32 binding);
    };
} // namespace OloEngine
