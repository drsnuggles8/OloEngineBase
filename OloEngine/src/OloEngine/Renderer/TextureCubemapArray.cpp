#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TextureCubemapArray.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTextureCubemapArray.h"
#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTransientResources.h"
#endif

namespace OloEngine
{
    Ref<TextureCubemapArray> TextureCubemapArray::Create(const CubemapArraySpecification& specification)
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
                // #691 Phase 8: the amendment (64) leftover — this assert
                // wedged the first --rhi=vulkan editor launch during
                // ReflectionProbeArray::Init.
#if OLO_WITH_VULKAN
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanTextureCubemapArray>::Create(specification);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan selected but no VulkanDevice is live!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLTextureCubemapArray>::Create(specification);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
