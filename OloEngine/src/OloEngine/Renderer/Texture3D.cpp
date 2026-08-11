#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Texture3D.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTexture3D.h"

#if OLO_WITH_VULKAN
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTransientResources.h"
#endif

namespace OloEngine
{
    Ref<Texture3D> Texture3D::Create(const Texture3DSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
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
                // A Vulkan resource cannot exist without a device, so fall
                // through to the assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanTexture3D>::Create(spec);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out) — cannot create a Vulkan 3D texture!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
                return Ref<OpenGLTexture3D>::Create(spec);
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
