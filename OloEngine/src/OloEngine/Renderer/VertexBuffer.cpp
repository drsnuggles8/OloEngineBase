#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/RenderCommand.h"
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

    ImmutableVertexBufferUpload::ImmutableVertexBufferUpload(u32 sizeBytes, u32 chunkBytes)
        : m_Size(sizeBytes), m_ChunkBytes(std::min(chunkBytes, sizeBytes))
    {
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
                m_Destination = Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(sizeBytes, 0u));
                m_Staging = Ref<OpenGLVertexBuffer>(new OpenGLVertexBuffer(m_ChunkBytes, GL_DYNAMIC_STORAGE_BIT));
                return;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                if (VulkanDevice::Get() != nullptr)
                {
                    m_Destination = Ref<VulkanVertexBuffer>::Create(sizeBytes);
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
        if (m_Staging)
        {
            // Immutable storage takes copies, not glNamedBufferSubData.
            m_Staging->SetData({ data, bytes });
            RenderCommand::CopyBufferSubData(m_Staging->GetRHIHandle(), m_Destination->GetRHIHandle(), 0u, m_Written, bytes);
        }
        else if (!RenderCommand::TryUploadBufferSubData(m_Destination->GetRHIHandle(), m_Written, bytes, data))
        {
            OLO_CORE_ERROR("ImmutableVertexBufferUpload: the upload of {} bytes at {} was refused", bytes, m_Written);
            m_Failed = true;
            return false;
        }
        m_Written += bytes;
        return true;
    }

    Ref<VertexBuffer> ImmutableVertexBufferUpload::Finish()
    {
        m_Staging = nullptr;
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
