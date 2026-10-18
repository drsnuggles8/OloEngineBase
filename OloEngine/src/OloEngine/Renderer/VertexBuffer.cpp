#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"

#include <chrono>
#include "Platform/OpenGL/OpenGLVertexBuffer.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanBufferResources.h"
#endif

namespace OloEngine
{

    Ref<VertexBuffer> VertexBuffer::Create(u32 size)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                // #691: real Vulkan factory arm. A Vulkan resource
                // cannot exist without a device, so fall through to the loud
                // assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanVertexBuffer>::Create(size);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(size));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<VertexBuffer> VertexBuffer::Create(f32* vertices, u32 size)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                // #691: real Vulkan factory arm. A Vulkan resource
                // cannot exist without a device, so fall through to the loud
                // assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanVertexBuffer>::Create(vertices, size);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(vertices, size));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<VertexBuffer> VertexBuffer::Create(const f32* vertices, u32 size)
    {
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                // #691: real Vulkan factory arm. A Vulkan resource
                // cannot exist without a device, so fall through to the loud
                // assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanVertexBuffer>::Create(vertices, size);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(vertices, size));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<VertexBuffer> VertexBuffer::Create(const void* data, u32 size)
    {
        // Runtime check: if size > 0, data must not be null
        // For uninitialized buffer creation, use Create(u32 size) instead
        OLO_CORE_ASSERT(size == 0 || data != nullptr,
                        "VertexBuffer::Create(): data cannot be null when size > 0. Use Create(u32 size) for uninitialized buffer creation.");

        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                // #691: real Vulkan factory arm. A Vulkan resource
                // cannot exist without a device, so fall through to the loud
                // assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanVertexBuffer>::Create(data, size);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(data, size));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<VertexBuffer> VertexBuffer::CreateImmutable(const void* data, u32 size)
    {
        OLO_CORE_ASSERT(size == 0 || data != nullptr, "VertexBuffer::CreateImmutable(): an immutable buffer needs its data now");

        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanVertexBuffer>::Create(data, size);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                // Storage flags 0: the GPU's copy is the only one. Even
                // GL_DYNAMIC_STORAGE_BIT brings the resident copy back on the
                // NVIDIA driver (measured on #1608).
                return Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(data, size, 0u));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    namespace
    {
        [[nodiscard]] u64 MicrosecondsSince(std::chrono::steady_clock::time_point start)
        {
            return static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        }
    } // namespace

    ImmutableVertexBufferUpload::ImmutableVertexBufferUpload(u32 sizeBytes, u32 chunkBytes)
        : m_Size(sizeBytes), m_ChunkBytes(std::min(chunkBytes, sizeBytes))
    {
        const auto start = std::chrono::steady_clock::now();
        if (sizeBytes == 0u || chunkBytes == 0u)
        {
            OLO_CORE_ERROR("ImmutableVertexBufferUpload: {} bytes in chunks of {} is not a buffer", sizeBytes, chunkBytes);
            m_Failed = true;
            return;
        }
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
            {
                auto destination = Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(sizeBytes, 0u));
                if (!destination->HasItsStorage())
                {
                    OLO_CORE_ERROR("ImmutableVertexBufferUpload: GL could not allocate a {}-byte buffer", sizeBytes);
                    m_Failed = true;
                    return;
                }
                m_Destination = destination;
                const RendererMemoryOwnerScope staging(kStagingOwner, MemoryLifetime::Staging);
                for (Ref<VertexBuffer>& buffer : m_Staging)
                {
                    buffer = Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(m_ChunkBytes, GL_DYNAMIC_STORAGE_BIT));
                }
                m_UploadMicroseconds += MicrosecondsSince(start);
                return;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                if (VulkanDevice::Get() != nullptr)
                {
                    m_Destination = Ref<VulkanVertexBuffer>::Create(sizeBytes);
                    m_UploadMicroseconds += MicrosecondsSince(start);
                    return;
                }
#endif
                break;
            }
            case RendererAPI::API::None:
                break;
        }
        OLO_CORE_ASSERT(false, "ImmutableVertexBufferUpload: no backend can create the buffer");
        m_Failed = true;
    }

    bool ImmutableVertexBufferUpload::Append(const void* data, u32 bytes)
    {
        if (m_Failed)
        {
            return false;
        }
        if (data == nullptr || bytes == 0u || bytes > m_ChunkBytes || bytes > m_Size - m_Written)
        {
            OLO_CORE_ERROR("ImmutableVertexBufferUpload: {} bytes at {} do not fit a {}-byte buffer in {}-byte chunks", bytes,
                           m_Written, m_Size, m_ChunkBytes);
            m_Failed = true;
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        bool written = false;
        if (m_Staging[0])
        {
            auto& staging = static_cast<OpenGLVertexBuffer&>(*m_Staging[m_NextStaging]);
            m_NextStaging ^= 1u;
            written = static_cast<OpenGLVertexBuffer&>(*m_Destination).UploadChunkThrough(staging, m_Written, data, bytes);
        }
        else
        {
#if OLO_WITH_VULKAN
            // Nothing has read the buffer yet, so a mapped one takes the bytes
            // directly; only an unmapped one stages them in the frame.
            written = static_cast<VulkanVertexBuffer&>(*m_Destination).WriteBeforeFirstUse(m_Written, data, bytes) ||
                      RenderCommand::TryUploadBufferSubData(m_Destination->GetRHIHandle(), m_Written, bytes, data);
#endif
        }
        m_UploadMicroseconds += MicrosecondsSince(start);
        if (!written)
        {
            OLO_CORE_ERROR("ImmutableVertexBufferUpload: the upload of {} bytes at {} of {} failed", bytes, m_Written, m_Size);
            m_Failed = true;
            return false;
        }
        m_Written += bytes;
        return true;
    }

    Ref<VertexBuffer> ImmutableVertexBufferUpload::Finish()
    {
        m_Staging = {};
        if (!m_Failed && m_Written != m_Size)
        {
            OLO_CORE_ERROR("ImmutableVertexBufferUpload: {} of {} bytes were written; the buffer is not built", m_Written,
                           m_Size);
            m_Failed = true;
        }
        if (m_Failed)
        {
            m_Destination = nullptr;
        }
        return std::move(m_Destination);
    }
} // namespace OloEngine
