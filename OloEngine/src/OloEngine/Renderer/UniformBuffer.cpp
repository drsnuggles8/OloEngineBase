#include "OloEnginePCH.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLUniformBuffer.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanBufferResources.h"
#endif

namespace OloEngine
{
    Ref<UniformBuffer> UniformBuffer::Create(u32 size, u32 binding)
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
                // #691 Phase 7: real Vulkan factory arm. A Vulkan resource
                // cannot exist without a device, so fall through to the loud
                // assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanUniformBuffer>::Create(size, binding);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<UniformBuffer>(new OpenGLUniformBuffer(size, binding));
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
