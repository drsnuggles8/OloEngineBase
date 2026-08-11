#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTextureCubemap.h"
#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanTransientResources.h"
#endif

namespace OloEngine
{
    Ref<TextureCubemap> TextureCubemap::Create(const std::vector<std::string>& facePaths)
    {
        OLO_CORE_ASSERT(facePaths.size() == 6, "Cubemap requires exactly 6 face textures!");

        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
                // Loading six face IMAGES needs the CPU upload path, which the
                // Vulkan cubemap does not have yet (#691 Phase 8) — so the
                // skybox/IBL cubemaps come back null and the sky renders
                // flat. Degrades to a warning rather than an assert so the
                // caller's null check decides.
                //
                // WARN-ONCE, because the caller RETRIES: an asset that fails
                // to load is re-requested every frame, and an unconditional
                // warn floods the log at frame rate (~1 line per frame per
                // cubemap) — which is how this was noticed. The failure is a
                // capability gap, not a per-frame event.
                static bool s_WarnedFacePathCubemap = false;
                if (!s_WarnedFacePathCubemap)
                {
                    s_WarnedFacePathCubemap = true;
                    OLO_CORE_WARN("[RHI/Vulkan] TextureCubemap::Create(facePaths) needs the CPU face-upload path "
                                  "(#691 Phase 8) — returning null (warned once; the skybox and IBL will be "
                                  "unlit/flat for this session)");
                }
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLTextureCubemap>::Create(facePaths);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<TextureCubemap> TextureCubemap::Create(const CubemapSpecification& specification)
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
                // A Vulkan resource cannot exist without a device, so fall
                // through to the assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanTextureCubemap>::Create(specification);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out) — cannot create a Vulkan cubemap!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLTextureCubemap>::Create(specification);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
