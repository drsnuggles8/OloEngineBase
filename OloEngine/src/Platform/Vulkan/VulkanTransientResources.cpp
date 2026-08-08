#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTransientResources.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace OloEngine
{
    namespace
    {
        // Kept in sync with VulkanDevice.cpp's / VulkanContext.cpp's copies
        // (all anonymous-namespace, trivially small — not worth a shared header).
        void VkCheck(VkResult result, const char* what)
        {
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(std::string("Vulkan bring-up: ") + what + " failed (VkResult " +
                                         std::to_string(static_cast<int>(result)) + ")");
            }
        }

        // Non-dispatchable Vulkan handles are pointers on 64-bit builds and
        // u64 on 32-bit ones — normalise either shape into the registry's u64.
        template<typename T>
        [[nodiscard]] u64 VkHandleToU64(T handle)
        {
            if constexpr (std::is_pointer_v<T>)
            {
                return static_cast<u64>(reinterpret_cast<std::uintptr_t>(handle));
            }
            else
            {
                return static_cast<u64>(handle);
            }
        }

        // ImageFormat -> VkFormat. Every ImageFormat member is covered; the
        // -Wswitch-with-no-default convention (ADR 0011) makes a new member a
        // compile warning here rather than a silent VK_FORMAT_UNDEFINED.
        //
        // Widening choices (documented, deliberate):
        //  - RGB8 / RGB32F widen to their RGBA siblings: 3-channel formats
        //    have no mandated optimal-tiling sampled/attachment support.
        //  - DEPTH24STENCIL8 maps to D32_SFLOAT_S8_UINT, not D24_UNORM_S8_UINT:
        //    Vulkan mandates support for at least one of the two and AMD ships
        //    only the D32 variant, so this is the portable pick (more depth
        //    precision, same aspects). Format-feature querying is Phase 6.
        //  - `srgb` is honoured only where the engine defines it (8-bit color
        //    and BC7), matching TextureSpecification::SRGB's contract.
        [[nodiscard]] VkFormat ImageFormatToVkFormat(ImageFormat format, bool srgb)
        {
            switch (format)
            {
                case ImageFormat::None:
                    break;
                case ImageFormat::R8:
                    return VK_FORMAT_R8_UNORM;
                case ImageFormat::R8UI:
                    return VK_FORMAT_R8_UINT;
                case ImageFormat::R16UI:
                    return VK_FORMAT_R16_UINT;
                case ImageFormat::RG16UI:
                    return VK_FORMAT_R16G16_UINT;
                case ImageFormat::RGB8:
                    return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                case ImageFormat::RGBA8:
                    return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
                case ImageFormat::RGBA16F:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case ImageFormat::RGBA32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case ImageFormat::R32F:
                    return VK_FORMAT_R32_SFLOAT;
                case ImageFormat::RG32F:
                    return VK_FORMAT_R32G32_SFLOAT;
                case ImageFormat::RGB32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case ImageFormat::DEPTH24STENCIL8:
                    return VK_FORMAT_D32_SFLOAT_S8_UINT;
                case ImageFormat::RG16F:
                    return VK_FORMAT_R16G16_SFLOAT;
                case ImageFormat::R32I:
                    return VK_FORMAT_R32_SINT;
                case ImageFormat::RG8:
                    return VK_FORMAT_R8G8_UNORM;
                case ImageFormat::BC7:
                    return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
                case ImageFormat::BC5:
                    return VK_FORMAT_BC5_UNORM_BLOCK;
                case ImageFormat::BC6H:
                    return VK_FORMAT_BC6H_UFLOAT_BLOCK;
            }

            OLO_CORE_ASSERT(false, "ImageFormatToVkFormat: unknown ImageFormat {}", static_cast<u32>(format));
            return VK_FORMAT_UNDEFINED;
        }

        [[nodiscard]] bool IsDepthImageFormat(ImageFormat format)
        {
            return format == ImageFormat::DEPTH24STENCIL8;
        }

        [[nodiscard]] bool IsSrgbVkFormat(VkFormat format)
        {
            return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_BC7_SRGB_BLOCK;
        }

        [[nodiscard]] VkSampleCountFlagBits SampleCountFromU32(u32 samples)
        {
            switch (samples)
            {
                case 0:
                case 1:
                    return VK_SAMPLE_COUNT_1_BIT;
                case 2:
                    return VK_SAMPLE_COUNT_2_BIT;
                case 4:
                    return VK_SAMPLE_COUNT_4_BIT;
                case 8:
                    return VK_SAMPLE_COUNT_8_BIT;
                case 16:
                    return VK_SAMPLE_COUNT_16_BIT;
                case 32:
                    return VK_SAMPLE_COUNT_32_BIT;
                case 64:
                    return VK_SAMPLE_COUNT_64_BIT;
                default:
                    OLO_CORE_WARN("VulkanTexture2D: unsupported sample count {} — falling back to 1", samples);
                    return VK_SAMPLE_COUNT_1_BIT;
            }
        }

        // Mirrors OpenGLTexture2D::CalculateFullMipCount.
        [[nodiscard]] u32 CalculateFullMipCount(u32 width, u32 height)
        {
            return static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(width, height))))) + 1;
        }

        // Mirrors the GL twin's mip derivation, including the spec mutations:
        // multisampling forces a single level, an explicit MipLevels wins,
        // GenerateMips selects the full chain, otherwise 1.
        [[nodiscard]] u32 DeriveMipLevels(TextureSpecification& spec, u32 width, u32 height)
        {
            if (spec.Samples > 1u)
            {
                spec.GenerateMips = false;
                spec.MipLevels = 1u;
                return 1u;
            }
            if (spec.MipLevels > 0u)
            {
                // Clamp an authored level count to the full chain the extents
                // support — Vulkan refuses mipLevels beyond
                // floor(log2(max(w,h)))+1 (VUID-VkImageCreateInfo-mipLevels-02255),
                // where GL silently tolerated the over-ask. Resize re-derives
                // through this same path, so a persisted over-large spec stays
                // bounded there too.
                return std::min(spec.MipLevels, CalculateFullMipCount(width, height));
            }
            if (spec.GenerateMips)
            {
                return CalculateFullMipCount(width, height);
            }
            return 1u;
        }

        // FramebufferTextureFormat -> ImageFormat, so VulkanFramebuffer can
        // build its attachments as real VulkanTexture2D instances (handles +
        // VulkanImageInfoRegistry entries come for free). Widening choices:
        //  - RGB16F -> RGBA16F: ImageFormat has no RGB16F member, and the
        //    Vulkan map would widen a 3-channel format anyway (see above).
        //  - DEPTH_COMPONENT32F -> DEPTH24STENCIL8: ImageFormat has no
        //    stencil-free depth member; the Vulkan map allocates
        //    D32_SFLOAT_S8_UINT for it, so a shadow depth attachment keeps its
        //    32-bit float depth aspect — the surplus stencil aspect is unused.
        // (Depth/ShadowDepth are enumerator aliases of the two depth members,
        // so this switch covers every distinct value.)
        [[nodiscard]] ImageFormat FramebufferFormatToImageFormat(FramebufferTextureFormat format)
        {
            switch (format)
            {
                case FramebufferTextureFormat::None:
                    break;
                case FramebufferTextureFormat::RGBA8:
                    return ImageFormat::RGBA8;
                case FramebufferTextureFormat::RGBA16F:
                    return ImageFormat::RGBA16F;
                case FramebufferTextureFormat::RGBA32F:
                    return ImageFormat::RGBA32F;
                case FramebufferTextureFormat::RGB16F:
                    return ImageFormat::RGBA16F;
                case FramebufferTextureFormat::RGB32F:
                    return ImageFormat::RGB32F;
                case FramebufferTextureFormat::RG16F:
                    return ImageFormat::RG16F;
                case FramebufferTextureFormat::RG32F:
                    return ImageFormat::RG32F;
                case FramebufferTextureFormat::RED_INTEGER:
                    return ImageFormat::R32I;
                case FramebufferTextureFormat::DEPTH24STENCIL8:
                    return ImageFormat::DEPTH24STENCIL8;
                case FramebufferTextureFormat::DEPTH_COMPONENT32F:
                    return ImageFormat::DEPTH24STENCIL8;
            }

            OLO_CORE_ASSERT(false, "FramebufferFormatToImageFormat: unknown FramebufferTextureFormat {}",
                            static_cast<u32>(format));
            return ImageFormat::None;
        }

        [[nodiscard]] bool IsDepthFramebufferFormat(FramebufferTextureFormat format)
        {
            return format == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                   format == FramebufferTextureFormat::DEPTH_COMPONENT32F;
        }
    } // namespace

// Phase 6 stub: warn ONCE per entry point, then no-op with a benign return.
// Each expansion gets its own function-local static, so no two entry points
// ever share a flag. Deliberately NOT an assert: TransientPool's acquire path
// must stay alive under --rhi=vulkan even when a caller pokes an
// upload/bind-shaped virtual.
#define OLO_VK_PHASE6_STUB(entryPoint)                                                      \
    do                                                                                      \
    {                                                                                       \
        static bool s_WarnedOnce = false;                                                   \
        if (!s_WarnedOnce)                                                                  \
        {                                                                                   \
            s_WarnedOnce = true;                                                            \
            OLO_CORE_WARN(entryPoint " is a Phase 6 concern — no-op under Phase 5 (#691)"); \
        }                                                                                   \
    } while (false)

    // =========================================================================
    // VulkanImageInfoRegistry
    // =========================================================================

    VulkanImageInfoRegistry& VulkanImageInfoRegistry::Get()
    {
        static auto* s_Instance = new VulkanImageInfoRegistry(); // deliberately leaked
        return *s_Instance;
    }

    void VulkanImageInfoRegistry::Register(VkImage image, const VulkanImageInfo& info)
    {
        if (image == VK_NULL_HANDLE)
        {
            return;
        }
        // Stamp every registration uniquely so a layout tracker can tell a
        // driver-recycled handle VALUE apart from the image it tracked — see
        // VulkanImageInfo::RegistrationId.
        static u64 s_NextRegistrationId = 0;
        m_Infos[image] = info;
        m_Infos[image].RegistrationId = ++s_NextRegistrationId;
    }

    const VulkanImageInfo* VulkanImageInfoRegistry::Lookup(VkImage image) const
    {
        const auto it = m_Infos.find(image);
        return it != m_Infos.end() ? &it->second : nullptr;
    }

    void VulkanImageInfoRegistry::Unregister(VkImage image)
    {
        m_Infos.erase(image);
    }

    // =========================================================================
    // VulkanDeferredReclaim
    // =========================================================================

    VulkanDeferredReclaim& VulkanDeferredReclaim::Get()
    {
        static auto* s_Instance = new VulkanDeferredReclaim(); // deliberately leaked
        return *s_Instance;
    }

    void VulkanDeferredReclaim::Enqueue(VkImage image, VmaAllocation allocation)
    {
        if (image == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE)
        {
            return;
        }
        m_Entries.push_back({ image, VK_NULL_HANDLE, allocation, m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkBuffer buffer, VmaAllocation allocation)
    {
        if (buffer == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE)
        {
            return;
        }
        m_Entries.push_back({ VK_NULL_HANDLE, buffer, allocation, m_Generation });
    }

    void VulkanDeferredReclaim::DestroyEntry(const Entry& entry)
    {
        // Image metadata retires at ACTUAL destroy time — a barrier emitted for
        // a still-enqueued image must keep resolving until the image is gone.
        if (entry.Image != VK_NULL_HANDLE)
        {
            VulkanImageInfoRegistry::Get().Unregister(entry.Image);
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            // Shutdown teardown race: the device (and with it the allocator)
            // is already gone. Dropping the entry leaks at process exit, which
            // beats calling into a destroyed allocator.
            OLO_CORE_WARN("VulkanDeferredReclaim: dropping a reclaim entry — VulkanDevice already shut down");
            return;
        }

        if (entry.Image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(device->GetAllocator(), entry.Image, entry.Allocation);
        }
        else if (entry.Buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(device->GetAllocator(), entry.Buffer, entry.Allocation);
        }
        else if (entry.Allocation != VK_NULL_HANDLE)
        {
            vmaFreeMemory(device->GetAllocator(), entry.Allocation);
        }
    }

    void VulkanDeferredReclaim::NotifyFrameCompleted()
    {
        ++m_Generation;

        std::erase_if(m_Entries, [this](const Entry& entry)
                      {
            if (m_Generation - entry.EnqueuedAtGeneration < kFramesInFlight)
            {
                return false;
            }
            DestroyEntry(entry);
            return true; });
    }

    void VulkanDeferredReclaim::FlushAll()
    {
        // Caller guarantees vkDeviceWaitIdle has already been done.
        for (const auto& entry : m_Entries)
        {
            DestroyEntry(entry);
        }
        m_Entries.clear();
    }

    // =========================================================================
    // VulkanTexture2D
    // =========================================================================

    VulkanTexture2D::VulkanTexture2D(const TextureSpecification& specification)
        // Vulkan refuses a zero extent outright (GL merely misbehaved), so
        // clamp defensively — pre-Resize 0-sized framebuffer specs reach here.
        : m_Specification(specification),
          m_Width(std::max(specification.Width, 1u)),
          m_Height(std::max(specification.Height, 1u))
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanTexture2D requires a live VulkanDevice");

        // Mirror the GL twin: block-compressed formats have no population path
        // through the spec ctor — they MUST come via the CompressedTextureImage
        // overload (Phase 6 for Vulkan). Refuse loudly but non-fatally.
        if (IsCompressedFormat(m_Specification.Format))
        {
            OLO_CORE_ERROR("VulkanTexture2D: block-compressed format {} cannot be created from a "
                           "TextureSpecification — the CompressedTextureImage overload is Phase 6",
                           static_cast<u32>(m_Specification.Format));
            m_IsLoaded = false;
            return;
        }

        m_Specification.Samples = std::max(m_Specification.Samples, 1u);
        m_MipLevels = DeriveMipLevels(m_Specification, m_Width, m_Height);

        CreateImage();
        m_IsLoaded = true;
    }

    VulkanTexture2D::~VulkanTexture2D()
    {
        // Retire the identity first (outstanding handles go stale), then hand
        // the native object to the deferred queue — NEVER vmaDestroyImage
        // inline, prior frames may still be executing. Destructors must not
        // let an exception escape (the reclaim enqueue can allocate): a
        // failed enqueue leaks one image until process exit, which beats
        // std::terminate.
        try
        {
            m_RHIHandle.Reset();
            ReleaseImage();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanTexture2D: release failed ({}) — leaking the image until process exit", e.what());
        }
    }

    void VulkanTexture2D::CreateImage()
    {
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTexture2D::CreateImage requires a live VulkanDevice");
        if (device == nullptr)
        {
            // The assert compiles out in Release; the factory arm guards this
            // path, but a Resize on a device that has since shut down must
            // fail loudly rather than dereference null.
            throw std::runtime_error("VulkanTexture2D::CreateImage: no live VulkanDevice");
        }

        const VkFormat format = ImageFormatToVkFormat(m_Specification.Format, m_Specification.SRGB);
        const bool isDepth = IsDepthImageFormat(m_Specification.Format);

        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (isDepth)
        {
            usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        else
        {
            usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            // STORAGE only where the format-feature contract allows it without
            // a per-format query (Phase 6): sRGB image views don't support
            // storage writes, and multisampled storage images need an opt-in
            // device feature.
            if (!IsSrgbVkFormat(format) && m_Specification.Samples == 1u)
            {
                usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { m_Width, m_Height, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = 1u;
        imageInfo.samples = SampleCountFromU32(m_Specification.Samples);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        VkCheck(vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr),
                "vmaCreateImage (VulkanTexture2D)");

        // HasStencil follows the RESOLVED VkFormat: the engine's combined
        // depth format lowers to D32_SFLOAT_S8_UINT, which carries a stencil
        // aspect — the orchestrator derives barrier aspect masks from this.
        VulkanImageInfoRegistry::Get().Register(m_Image, VulkanImageInfo{
                                                             .Format = format,
                                                             .MipLevels = m_MipLevels,
                                                             .ArrayLayers = 1u,
                                                             .HasDepth = isDepth,
                                                             .HasStencil = isDepth,
                                                         });

        m_RHIHandle.Sync(RHI::ResourceKind::Texture, VkHandleToU64(m_Image), RHI::Backend::Vulkan);
    }

    void VulkanTexture2D::ReleaseImage()
    {
        if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            // The info-registry entry retires inside the reclaim queue at
            // actual-destroy time, not here.
            VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
        }
    }

    void VulkanTexture2D::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0u || height == 0u)
        {
            OLO_CORE_WARN("VulkanTexture2D::Resize: ignoring zero extent {}x{}", width, height);
            return;
        }
        if (width == m_Width && height == m_Height && m_Image != VK_NULL_HANDLE)
        {
            return;
        }

        ReleaseImage();

        m_Width = width;
        m_Height = height;
        m_Specification.Width = width;
        m_Specification.Height = height;
        m_MipLevels = DeriveMipLevels(m_Specification, m_Width, m_Height);

        // Sync inside CreateImage PRESERVES the identity — same object, new
        // storage, matching the GL twin's recreate-in-place semantics.
        CreateImage();
    }

    void VulkanTexture2D::SetData(void* data, u32 size)
    {
        (void)data;
        (void)size;
        OLO_VK_PHASE6_STUB("VulkanTexture2D::SetData");
    }

    void VulkanTexture2D::SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize)
    {
        (void)x;
        (void)y;
        (void)width;
        (void)height;
        (void)data;
        (void)dataSize;
        OLO_VK_PHASE6_STUB("VulkanTexture2D::SubImage");
    }

    void VulkanTexture2D::Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
    {
        (void)path;
        (void)width;
        (void)height;
        (void)data;
        (void)channels;
        OLO_VK_PHASE6_STUB("VulkanTexture2D::Invalidate");
    }

    void VulkanTexture2D::Bind(u32 slot) const
    {
        (void)slot;
        OLO_VK_PHASE6_STUB("VulkanTexture2D::Bind");
    }

    bool VulkanTexture2D::GetData(std::vector<u8>& outData, u32 mipLevel) const
    {
        (void)mipLevel;
        outData.clear();
        OLO_VK_PHASE6_STUB("VulkanTexture2D::GetData");
        return false;
    }

    // =========================================================================
    // VulkanFramebuffer
    // =========================================================================

    VulkanFramebuffer::VulkanFramebuffer(const FramebufferSpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanFramebuffer requires a live VulkanDevice");

        for (const auto& attachmentSpec : m_Specification.Attachments.Attachments)
        {
            if (IsDepthFramebufferFormat(attachmentSpec.TextureFormat))
            {
                m_DepthAttachmentSpecification = attachmentSpec;
            }
            else if (attachmentSpec.TextureFormat != FramebufferTextureFormat::None)
            {
                m_ColorAttachmentSpecifications.push_back(attachmentSpec);
            }
        }

        CreateAttachments();

        // The framebuffer's own identity: native = 0 because under dynamic
        // rendering no VkFramebuffer object exists to name (render passes are
        // Phase 6). The attachments carry their own nonzero-native handles.
        m_RHIHandle.Adopt(RHI::ResourceKind::Framebuffer, 0u, RHI::Backend::Vulkan);
    }

    VulkanFramebuffer::~VulkanFramebuffer()
    {
        // Retire the framebuffer identity; the attachment Refs release next
        // and each texture enqueues its image on VulkanDeferredReclaim.
        m_RHIHandle.Reset();
    }

    void VulkanFramebuffer::CreateAttachments()
    {
        // Replacing the Refs drops the old attachments — their destructors
        // route the images through VulkanDeferredReclaim, never an inline
        // destroy.
        m_ColorAttachments.clear();
        m_DepthAttachment = nullptr;

        // A 0-sized spec is legal in the engine (framebuffers are routinely
        // resized before first use); Vulkan refuses a zero extent, so the
        // attachment textures clamp to 1x1 until Resize provides real
        // dimensions. m_Specification keeps the authored values.
        const u32 width = std::max(m_Specification.Width, 1u);
        const u32 height = std::max(m_Specification.Height, 1u);

        const auto makeAttachmentSpec = [&](FramebufferTextureFormat format)
        {
            TextureSpecification texSpec;
            texSpec.Width = width;
            texSpec.Height = height;
            texSpec.Format = FramebufferFormatToImageFormat(format);
            texSpec.GenerateMips = false;
            texSpec.MipLevels = 1u;
            texSpec.Samples = std::max(m_Specification.Samples, 1u);
            return texSpec;
        };

        m_ColorAttachments.reserve(m_ColorAttachmentSpecifications.size());
        for (const auto& attachmentSpec : m_ColorAttachmentSpecifications)
        {
            m_ColorAttachments.push_back(Ref<VulkanTexture2D>::Create(makeAttachmentSpec(attachmentSpec.TextureFormat)));
        }

        if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
        {
            m_DepthAttachment = Ref<VulkanTexture2D>::Create(makeAttachmentSpec(m_DepthAttachmentSpecification.TextureFormat));
        }
    }

    void VulkanFramebuffer::Bind()
    {
        // Will set the orchestrator's VulkanRendererAPI current-render-target
        // state once that lands.
        OLO_VK_PHASE6_STUB("VulkanFramebuffer::Bind");
    }

    void VulkanFramebuffer::Unbind()
    {
        OLO_VK_PHASE6_STUB("VulkanFramebuffer::Unbind");
    }

    void VulkanFramebuffer::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0u || height == 0u)
        {
            OLO_CORE_WARN("VulkanFramebuffer::Resize: ignoring zero extent {}x{}", width, height);
            return;
        }

        m_Specification.Width = width;
        m_Specification.Height = height;

        // Physical resize implies render viewport == physical size (same
        // contract as the GL twin).
        m_RenderViewportWidth = 0u;
        m_RenderViewportHeight = 0u;

        // The attachments genuinely become NEW objects with NEW handles —
        // anything still holding the old ones has to see them go stale (same
        // contract as the GL twin). The old images auto-enqueue on
        // VulkanDeferredReclaim via their destructors.
        CreateAttachments();
    }

    void VulkanFramebuffer::SetRenderViewportSize(u32 width, u32 height)
    {
        m_RenderViewportWidth = width;
        m_RenderViewportHeight = height;
    }

    int VulkanFramebuffer::ReadPixel(u32 attachmentIndex, int x, int y)
    {
        (void)attachmentIndex;
        (void)x;
        (void)y;
        OLO_VK_PHASE6_STUB("VulkanFramebuffer::ReadPixel");
        return -1; // the entity-picking "nothing here" value
    }

    void VulkanFramebuffer::ClearAttachment(u32 attachmentIndex, int value)
    {
        (void)attachmentIndex;
        (void)value;
        OLO_VK_PHASE6_STUB("VulkanFramebuffer::ClearAttachment(int)");
    }

    void VulkanFramebuffer::ClearAttachment(u32 attachmentIndex, const glm::vec4& value)
    {
        (void)attachmentIndex;
        (void)value;
        OLO_VK_PHASE6_STUB("VulkanFramebuffer::ClearAttachment(vec4)");
    }

    void VulkanFramebuffer::ClearAllAttachments(const glm::vec4& clearColor, int entityIdClear)
    {
        (void)clearColor;
        (void)entityIdClear;
        OLO_VK_PHASE6_STUB("VulkanFramebuffer::ClearAllAttachments");
    }

    RHI::ResourceHandle VulkanFramebuffer::GetColorAttachmentHandle(u32 index) const
    {
        OLO_CORE_ASSERT(index < m_ColorAttachments.size());
        if (index >= m_ColorAttachments.size())
        {
            return {};
        }
        return m_ColorAttachments[index]->GetRHIHandle();
    }

    RHI::ResourceHandle VulkanFramebuffer::GetDepthAttachmentHandle() const
    {
        return m_DepthAttachment ? m_DepthAttachment->GetRHIHandle() : RHI::ResourceHandle{};
    }

    void VulkanFramebuffer::AttachDepthTextureArrayLayer(RHI::ResourceHandle textureArray, u32 layer)
    {
        (void)textureArray;
        (void)layer;
        // Shadow-cascade rendering needs the render-pass/PSO path.
        OLO_VK_PHASE6_STUB("VulkanFramebuffer::AttachDepthTextureArrayLayer");
    }

    Ref<VulkanTexture2D> VulkanFramebuffer::GetColorAttachmentImage(u32 index) const
    {
        OLO_CORE_ASSERT(index < m_ColorAttachments.size());
        if (index >= m_ColorAttachments.size())
        {
            return nullptr;
        }
        return m_ColorAttachments[index];
    }

    // =========================================================================
    // VulkanStorageBuffer
    // =========================================================================

    VulkanStorageBuffer::VulkanStorageBuffer(u32 size, u32 binding, StorageBufferUsage usage)
        : m_Size(size), m_Binding(binding), m_Usage(usage)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanStorageBuffer requires a live VulkanDevice");

        CreateBuffer();
    }

    VulkanStorageBuffer::~VulkanStorageBuffer()
    {
        // Identity first, then the deferred queue — never vmaDestroyBuffer
        // inline (prior frames may still be executing). No exception may
        // escape a destructor: a failed enqueue leaks one buffer until
        // process exit, which beats std::terminate.
        try
        {
            m_RHIHandle.Reset();
            ReleaseBuffer();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("~VulkanStorageBuffer: release failed ({}) — leaking the buffer until process exit", e.what());
        }
    }

    void VulkanStorageBuffer::CreateBuffer()
    {
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanStorageBuffer::CreateBuffer requires a live VulkanDevice");
        if (device == nullptr)
        {
            throw std::runtime_error("VulkanStorageBuffer::CreateBuffer: no live VulkanDevice");
        }

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        // Vulkan refuses a zero-sized buffer; clamp defensively (GetSize keeps
        // reporting the authored size).
        bufferInfo.size = std::max<VkDeviceSize>(m_Size, 1u);
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        // DynamicDraw means "CPU writes, GPU reads": let VMA prefer a
        // host-writable (BAR/host-visible) placement when one exists, falling
        // back to device-local + a transfer path otherwise — which is exactly
        // the upload shape Phase 6's SetData will need. DynamicCopy is
        // GPU-writes/GPU-reads and stays pure device-local.
        if (m_Usage == StorageBufferUsage::DynamicDraw)
        {
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT;
        }

        VkCheck(vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &m_Buffer, &m_Allocation, nullptr),
                "vmaCreateBuffer (VulkanStorageBuffer)");

        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, VkHandleToU64(m_Buffer), RHI::Backend::Vulkan);
    }

    void VulkanStorageBuffer::ReleaseBuffer()
    {
        if (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_Buffer, m_Allocation);
            m_Buffer = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
        }
    }

    void VulkanStorageBuffer::Bind() const
    {
        OLO_VK_PHASE6_STUB("VulkanStorageBuffer::Bind");
    }

    void VulkanStorageBuffer::Unbind() const
    {
        OLO_VK_PHASE6_STUB("VulkanStorageBuffer::Unbind");
    }

    void VulkanStorageBuffer::SetData(const void* data, u32 size, u32 offset)
    {
        (void)data;
        (void)size;
        (void)offset;
        OLO_VK_PHASE6_STUB("VulkanStorageBuffer::SetData");
    }

    void VulkanStorageBuffer::GetData(void* outData, u32 size, u32 offset) const
    {
        (void)offset;
        OLO_VK_PHASE6_STUB("VulkanStorageBuffer::GetData");
        // Benign return: callers read DEFINED memory (zeros), not garbage.
        if (outData != nullptr && size > 0u)
        {
            std::memset(outData, 0, size);
        }
    }

    void VulkanStorageBuffer::ClearData()
    {
        OLO_VK_PHASE6_STUB("VulkanStorageBuffer::ClearData");
    }

    void VulkanStorageBuffer::Resize(u32 newSize)
    {
        OLO_PROFILE_FUNCTION();

        if (newSize == m_Size && m_Buffer != VK_NULL_HANDLE)
        {
            return;
        }

        // Same contract as the GL twin: Resize invalidates existing data.
        ReleaseBuffer();
        m_Size = newSize;
        // Sync inside CreateBuffer PRESERVES the identity — same object, new
        // storage.
        CreateBuffer();
    }

#undef OLO_VK_PHASE6_STUB
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
