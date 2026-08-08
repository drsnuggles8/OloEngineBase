#include "OloEnginePCH.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLStorageBuffer.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanTransientResources.h"
#endif

namespace OloEngine
{
    Ref<StorageBuffer> StorageBuffer::Create(u32 size, u32 binding, StorageBufferUsage usage)
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
                // #691 Phase 5: the TransientPool's attribute-only path. A
                // Vulkan resource cannot exist without a device, so fall
                // through to the loud assert when none is up. Raw-new matches
                // this factory's OpenGL arm.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<StorageBuffer>(new VulkanStorageBuffer(size, binding, usage));
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out) — cannot create a Vulkan storage buffer!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<StorageBuffer>(new OpenGLStorageBuffer(size, binding, usage));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
