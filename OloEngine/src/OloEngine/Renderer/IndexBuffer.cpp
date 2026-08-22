#include "OloEnginePCH.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLIndexBuffer.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanBufferResources.h"
#endif

namespace OloEngine
{
    Ref<IndexBuffer> IndexBuffer::Create(u32* indices, u32 size)
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
                    return Ref<VulkanIndexBuffer>::Create(indices, size);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<IndexBuffer>(new OpenGLIndexBuffer(indices, size));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
