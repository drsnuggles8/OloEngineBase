#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Texture.h"

#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTexture.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanTransientResources.h"
#endif

namespace OloEngine
{
    std::filesystem::path Texture2D::ResolveStoredSourcePath(std::string_view sourcePath)
    {
        if (sourcePath.empty())
            return {};

        std::filesystem::path path(sourcePath);
        if (!path.is_relative())
            return path;

        // A relative path is only openable by luck: it resolves against the process
        // working directory, which is not the project root. Prefer the project-rooted
        // spelling the asset system stores, and only when that file is really there —
        // a texture created from a genuinely CWD-relative path still reloads.
        std::error_code ec;
        const bool haveProject = Project::GetActive() != nullptr;
        if (haveProject)
        {
            if (std::filesystem::path projectRooted = Project::GetProjectDirectory() / path;
                std::filesystem::exists(projectRooted, ec))
            {
                return projectRooted;
            }
        }

        if (std::filesystem::exists(path, ec))
            return path;

        // Neither base has the file. Say so and refuse — handing the relative path to
        // the loader anyway would read against the CWD, which is the silent failure
        // this helper exists to remove (#1067).
        OLO_CORE_ERROR("Texture2D::ResolveStoredSourcePath: cannot resolve relative source path '{}' ({}); "
                       "refusing to re-read it against the process working directory",
                       sourcePath,
                       haveProject ? "not found under the active project directory either" : "no active project");
        return {};
    }

    Ref<Texture2D> Texture2D::Create(const TextureSpecification& specification)
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
                // #691: the TransientPool's attribute-only path. A
                // Vulkan resource cannot exist without a device, so fall
                // through to the loud assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanTexture2D>::Create(specification);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out) — cannot create a Vulkan texture!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLTexture2D>::Create(specification);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<Texture2D> Texture2D::Create(const CompressedTextureImage& compressedImage)
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
                // Block-compressed upload (the BCn staging path) is #691
                // Degrading to null rather than asserting keeps an
                // asset that happens to be compressed from killing the app —
                // the caller falls back to its missing-texture handling
                // (see asset-degradation-and-constructor-preconditions.md).
                static bool s_Warned = false;
                if (!s_Warned)
                {
                    s_Warned = true;
                    OLO_CORE_WARN("[RHI/Vulkan] compressed-texture upload is not implemented (#691) — "
                                  "these textures load as null");
                }
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLTexture2D>::Create(compressedImage);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }

    Ref<Texture2D> Texture2D::Create(const std::string& path, bool srgb, const std::string& identityPath)
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
                // #691: file-load arm (stbi + one-shot upload).
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanTexture2D>::Create(path, srgb, identityPath);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out)!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
            {
                return Ref<OpenGLTexture2D>::Create(path, srgb, identityPath);
            }
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine
