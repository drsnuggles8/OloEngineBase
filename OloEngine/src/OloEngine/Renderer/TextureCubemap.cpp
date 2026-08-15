#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTextureCubemap.h"
#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanTransientResources.h"

#include <stb_image/stb_image.h>
#endif

namespace OloEngine
{
#if OLO_WITH_VULKAN
    namespace
    {
        // The Vulkan arm of the face-path load (#691 Phase 8): six stbi loads
        // into the spec-constructed cubemap via the CPU face upload. Mirrors
        // OpenGLTextureCubemap::LoadFaces' contract — faces are NOT
        // vertically flipped (unlike Texture2D), extent comes from face 0,
        // and every face must match it (pixels are force-expanded to RGBA8).
        Ref<TextureCubemap> LoadVulkanCubemapFromFacePaths(const std::vector<std::string>& facePaths)
        {
            stbi_set_flip_vertically_on_load_thread(0);

            Ref<TextureCubemap> cubemap;
            u32 faceWidth = 0;
            u32 faceHeight = 0;
            int channels = 0;
            for (u32 i = 0; i < 6u; ++i)
            {
                int width = 0;
                int height = 0;
                int fileChannels = 0;
                // Force RGBA: stbi expands 1/2/3-channel files, so the byte
                // size below always matches the RGBA8 spec (a native-channel
                // load handed SetFaceDataMip a texel size the format doesn't
                // describe for grey / grey-alpha faces).
                stbi_uc* pixels = stbi_load(facePaths[i].c_str(), &width, &height, &fileChannels, 4);
                if (pixels == nullptr)
                {
                    OLO_CORE_ERROR("[RHI/Vulkan] cubemap face '{}' failed to load: {}", facePaths[i],
                                   stbi_failure_reason());
                    return nullptr;
                }
                if (i == 0u)
                {
                    faceWidth = static_cast<u32>(width);
                    faceHeight = static_cast<u32>(height);
                    channels = fileChannels;
                    CubemapSpecification spec;
                    spec.Width = faceWidth;
                    spec.Height = faceHeight;
                    spec.Format = ImageFormat::RGBA8;
                    spec.GenerateMips = true;
                    cubemap = Ref<VulkanTextureCubemap>::Create(spec);
                }
                if (cubemap == nullptr)
                {
                    OLO_CORE_ERROR("[RHI/Vulkan] cubemap creation failed for '{}' ({}x{})", facePaths[i], faceWidth,
                                   faceHeight);
                    stbi_image_free(pixels);
                    return nullptr;
                }
                if (static_cast<u32>(width) != faceWidth || static_cast<u32>(height) != faceHeight ||
                    fileChannels != channels)
                {
                    OLO_CORE_ERROR("[RHI/Vulkan] cubemap face '{}' is {}x{}x{} but face 0 was {}x{}x{}", facePaths[i],
                                   width, height, fileChannels, faceWidth, faceHeight, channels);
                    stbi_image_free(pixels);
                    return nullptr;
                }
                const u32 size = faceWidth * faceHeight * 4u;
                // Upload mip 0 only per face; one mip regeneration afterwards
                // (SetFaceData would regenerate after EVERY face).
                const bool uploaded = cubemap->SetFaceDataMip(i, 0u, pixels, size);
                stbi_image_free(pixels);
                if (!uploaded)
                {
                    return nullptr;
                }
            }
            cubemap->GenerateMipmaps();
            return cubemap;
        }
    } // namespace
#endif

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
#if OLO_WITH_VULKAN
                if (VulkanDevice::Get() != nullptr)
                {
                    return LoadVulkanCubemapFromFacePaths(facePaths);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up — cannot create a Vulkan cubemap!");
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
