#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VertexArray.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLVertexArray.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanBufferResources.h"
#endif

namespace OloEngine
{
    Ref<VertexArray> VertexArray::Create()
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
                    return Ref<VulkanVertexArray>::Create();
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<VertexArray>(new OpenGLVertexArray());
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
