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
#include <limits>
#include <memory>
#include <utility>

namespace OloEngine
{
    namespace
    {
        Texture2D::ReloadGate s_ReloadGate = nullptr;
        constexpr auto kMaxDecodedBytes = static_cast<u64>(std::numeric_limits<i32>::max());

        bool CopyDecodedPixels(Texture2D::DecodedImage& out, const u8* pixels, int width, int height, int channels,
                               bool decodeSceneColor)
        {
            if (!pixels || width <= 0 || height <= 0 || channels < 1 || channels > 4)
                return false;
            const bool expandGray = decodeSceneColor && channels < 3;
            const int outputChannels = expandGray ? channels + 2 : channels;
            const u64 count = static_cast<u64>(width) * height;
            const u64 bytes = count * outputChannels;
            if (bytes > kMaxDecodedBytes)
                return false;
            out.Width = static_cast<u32>(width);
            out.Height = static_cast<u32>(height);
            out.Channels = static_cast<u32>(outputChannels);
            out.Pixels.SetNumUninitialized(static_cast<i32>(bytes));
            if (!expandGray)
            {
                std::memcpy(out.Pixels.GetData(), pixels, static_cast<sizet>(bytes));
                return true;
            }

            // GPUs only expose sRGB formats for RGB(A) data. Keep the alpha
            // component straight and expand the gray channel to all three colors.
            auto* destination = out.Pixels.GetData();
            for (u64 index = 0; index < count; ++index)
            {
                destination[0] = destination[1] = destination[2] = pixels[0];
                if (channels == 2)
                    destination[3] = pixels[1];
                destination += outputChannels;
                pixels += channels;
            }
            return true;
        }
    } // namespace

    void Texture2D::SetReloadGate(const ReloadGate gate)
    {
        s_ReloadGate = gate;
    }

    bool Texture2D::ReloadInPlace(Ref<Texture2D> texture, const std::filesystem::path& sourcePath)
    {
        if (!texture)
            return false;
        if (!s_ReloadGate && sourcePath.empty())
            return texture->Reload();

        DecodedImage image;
        const bool decoded = texture->DecodeReloadSource(image, sourcePath);
        if (s_ReloadGate)
            return s_ReloadGate(texture, decoded ? std::move(image) : DecodedImage{});
        return decoded && texture->ApplyReload(image);
    }

    bool Texture2D::DecodeReloadSource(DecodedImage& out, const std::filesystem::path& sourcePath) const
    {
        OLO_PROFILE_FUNCTION();

        // Only the uncompressed upload path can replace storage in place.
        if (IsCompressedFormat(GetSpecification().Format) || (sourcePath.empty() && GetPath().empty()))
            return false;
        const auto readPath = sourcePath.empty() ? ResolveStoredSourcePath(GetPath()) : sourcePath;
        if (readPath.empty())
            return false;

        int width = 0;
        int height = 0;
        int channels = 0;
        // The backends' loaders flip on load; a reload must match or the texture mirrors.
        ::stbi_set_flip_vertically_on_load_thread(1);
        const std::unique_ptr<stbi_uc, decltype(&::stbi_image_free)> data{
            ::stbi_load(readPath.string().c_str(), &width, &height, &channels, 0), &::stbi_image_free
        };
        ::stbi_set_flip_vertically_on_load_thread(0);
        if (!CopyDecodedPixels(out, data.get(), width, height, channels, m_DecodeSceneColor))
        {
            OLO_CORE_ERROR("Texture2D::DecodeReloadSource: failed to re-read '{}' (from '{}')", GetPath(), readPath.string());
            return false;
        }
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

    Ref<Texture2D> Texture2D::CreateFromEncodedImage(std::span<const u8> bytes, bool srgb, bool generateMips,
                                                     std::string_view identityPath, bool preserveAlphaCoverage)
    {
        if (bytes.empty() || bytes.size() > kMaxDecodedBytes)
            return nullptr;

        int width = 0;
        int height = 0;
        int channels = 0;
        ::stbi_set_flip_vertically_on_load_thread(1);
        const std::unique_ptr<stbi_uc, decltype(&::stbi_image_free)> pixels{
            ::stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 0),
            &::stbi_image_free
        };
        ::stbi_set_flip_vertically_on_load_thread(0);
        DecodedImage image;
        if (!CopyDecodedPixels(image, pixels.get(), width, height, channels, srgb))
        {
            OLO_CORE_ERROR("Texture2D: could not decode embedded or loose image '{}'", identityPath);
            return nullptr;
        }
        switch (Renderer::GetAPI())
        {
            case RendererAPI::API::OpenGL:
                return Ref<OpenGLTexture2D>::Create(image, srgb, generateMips, identityPath, preserveAlphaCoverage);
            case RendererAPI::API::Vulkan:
#if OLO_WITH_VULKAN
                if (VulkanDevice::Get() != nullptr)
                    return Ref<VulkanTexture2D>::Create(image, srgb, generateMips, identityPath, preserveAlphaCoverage);
#endif
                break;
            case RendererAPI::API::None:
                break;
        }
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
