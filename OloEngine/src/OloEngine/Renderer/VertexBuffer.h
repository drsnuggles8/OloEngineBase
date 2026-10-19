#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>
#include <type_traits>
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Ref.h"

namespace OloEngine
{
    // TODO(olbu): Add Create() functions for the new constructors of OpenGLVertexBuffer
    class VertexBuffer : public RefCounted
    {
      public:
        virtual ~VertexBuffer() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void SetData(const VertexData& data) = 0;

        [[nodiscard("Store this!")]] virtual const BufferLayout& GetLayout() const = 0;
        virtual void SetLayout(const BufferLayout& layout) = 0;

        [[nodiscard("Store this!")]] virtual u32 GetBufferHandle() const = 0;
        [[nodiscard]] virtual RHI::ResourceHandle GetRHIHandle() const = 0;
        // Zero when the active backend does not expose buffer device addresses.
        [[nodiscard]] virtual u64 GetDeviceAddress() const = 0;

        static Ref<VertexBuffer> Create(u32 size);
        static Ref<VertexBuffer> Create(f32* vertices, u32 size);
        static Ref<VertexBuffer> Create(const f32* vertices, u32 size);
        static Ref<VertexBuffer> Create(const void* data, u32 size);
        // WRITE-ONCE: filled here and never again; SetData on it is an error.
        // Create(data, size) is mutable storage on GL, and the driver keeps a
        // resident system-memory copy of every mutable buffer, so a 1.5 GB
        // groom stream cost 1.5 GB of the process's working set as well as
        // its VRAM (#1608). Immutable storage keeps that copy out. Vulkan
        // buffers are device-local either way, so there it is Create.
        static Ref<VertexBuffer> CreateImmutable(const void* data, u32 size);

        template<typename T, typename = std::enable_if_t<
                                 (std::is_integral_v<T> || std::is_enum_v<T>) && !std::is_same_v<T, u32>>>
        static Ref<VertexBuffer> Create(T, u32) = delete;
    };

    // A WRITE-ONCE vertex buffer filled in consecutive chunks (#1608), for a
    // stream too large to hold whole on the CPU while it uploads: what
    // CreateImmutable makes, without the caller ever holding all of its data.
    // GL: immutable storage with no flags, filled by copies from two refillable
    // staging buffers of one chunk each, used in turn so a chunk's write does
    // not wait on the previous chunk's copy (flags 0 refuse
    // glNamedBufferSubData), every step checked for a GL error. Vulkan: a
    // device-local buffer, written straight into its mapping when it has one
    // and by ordered uploads when it does not.
    class ImmutableVertexBufferUpload
    {
      public:
        // The owner the staging buffers are booked to, so a caller measuring
        // its own resident delta can leave them out.
        static constexpr std::string_view kStagingOwner = "ImmutableVertexBufferUpload staging";

        ImmutableVertexBufferUpload(u32 sizeBytes, u32 chunkBytes);

        // Appends `bytes` (at most one chunk) after what came before. False,
        // and logged, past the end or once anything failed.
        [[nodiscard]] bool Append(const void* data, u32 bytes);
        // The buffer once exactly sizeBytes were appended; null, and logged,
        // otherwise. Releases the staging buffers either way.
        [[nodiscard]] Ref<VertexBuffer> Finish();

        // Wall time spent creating, filling and finishing the buffer: the
        // upload's share of a build that interleaves it with its own work.
        [[nodiscard]] u64 GetUploadMicroseconds() const
        {
            return m_UploadMicroseconds;
        }

      private:
        Ref<VertexBuffer> m_Destination;
        std::array<Ref<VertexBuffer>, 2> m_Staging;
        u32 m_NextStaging = 0;
        u32 m_Size = 0;
        u32 m_ChunkBytes = 0;
        u32 m_Written = 0;
        u64 m_UploadMicroseconds = 0;
        bool m_Failed = false;
    };
} // namespace OloEngine
