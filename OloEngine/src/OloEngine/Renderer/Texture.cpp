#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Texture.h"

#include "OloEngine/Project/ContentPath.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Platform/OpenGL/OpenGLTexture.h"

#if OLO_WITH_VULKAN
// Sanctioned factory-include pattern (rhi-abstraction-boundary.md): this
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers.
#include "Platform/Vulkan/VulkanTransientResources.h"
#endif

#include <stb_image/stb_image.h>

#include <cstring>

namespace OloEngine
{
    namespace
    {
        Texture2D::ReloadGate s_ReloadGate = nullptr;
    } // namespace

    void Texture2D::SetReloadGate(const ReloadGate gate)
    {
        s_ReloadGate = gate;
    }

    bool Texture2D::ReloadInPlace(Ref<Texture2D> texture)
    {
        if (!texture)
            return false;
        return s_ReloadGate ? s_ReloadGate(texture) : texture->Reload();
    }

    bool Texture2D::DecodeReloadSource(DecodedImage& out) const
    {
        OLO_PROFILE_FUNCTION();

        // The same refusals as the backends' Reload(): a cooked container is a build
        // artifact, and only the uncompressed upload path can replace storage in place.
        if (IsCompressedFormat(GetSpecification().Format) || GetPath().empty())
            return false;
        const std::filesystem::path readPath = ResolveStoredSourcePath(GetPath());
        if (readPath.empty())
            return false;

        int width = 0;
        int height = 0;
        int channels = 0;
        // The backends' loaders flip on load; a reload must match or the texture mirrors.
        ::stbi_set_flip_vertically_on_load_thread(1);
        stbi_uc* data = ::stbi_load(readPath.string().c_str(), &width, &height, &channels, 0);
        ::stbi_set_flip_vertically_on_load_thread(0);
        if (!data || width <= 0 || height <= 0 || channels <= 0)
        {
            OLO_CORE_ERROR("Texture2D::DecodeReloadSource: failed to re-read '{}' (from '{}')", GetPath(), readPath.string());
            if (data)
                ::stbi_image_free(data);
            return false;
        }
        out.Width = static_cast<u32>(width);
        out.Height = static_cast<u32>(height);
        out.Channels = static_cast<u32>(channels);
        const sizet bytes = static_cast<sizet>(out.Width) * out.Height * out.Channels;
        out.Pixels.SetNumUninitialized(static_cast<i32>(bytes));
        std::memcpy(out.Pixels.GetData(), data, bytes);
        ::stbi_image_free(data);
        return true;
    }

    bool Texture2D::ApplyReload(const DecodedImage& image)
    {
        OLO_PROFILE_FUNCTION();
        if (!image.IsValid() || IsCompressedFormat(GetSpecification().Format))
            return false;
        // The path is copied first: Invalidate stores it into the member it may point at.
        const std::string path(GetPath());
        Invalidate(path, image.Width, image.Height, image.Pixels.GetData(), image.Channels);
        return IsLoaded();
    }

    std::filesystem::path Texture2D::ResolveStoredSourcePath(std::string_view sourcePath)
    {
        // One implementation for every stored content path (#1496): the path's
        // spelling picks the project directory or the working directory, never
        // both; a file missing there is an error and an empty result.
        return ResolveContentPath(sourcePath);
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
#if OLO_WITH_VULKAN
                // The cooked chain uploads as-is (#1453). A device that cannot
                // sample the format gets an UNLOADED texture and a named error,
                // not null: IsLoaded() is what the callers' missing-texture
                // handling already reads.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanTexture2D>::Create(compressedImage);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out) — cannot create a Vulkan texture!");
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
