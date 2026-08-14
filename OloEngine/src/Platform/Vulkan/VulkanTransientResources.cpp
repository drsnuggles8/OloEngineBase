#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTransientResources.h"

#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "Platform/Vulkan/VulkanBindingState.h"
#include "Platform/Vulkan/VulkanBufferResources.h"
#include "Platform/Vulkan/VulkanDescriptorSlotCache.h"
#include "Platform/Vulkan/VulkanImageLayoutTracker.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanOneShot.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#include <stb_image/stb_image.h>

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

        // Bytes per pixel of the ENGINE format's client data — what a caller
        // hands SetData (matches the GL twin's upload contract, so RGB8 is 3,
        // not the widened image's 4). 0 = no client-upload path (depth,
        // compressed, unknown).
        [[nodiscard]] u32 EngineFormatClientBpp(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::None:
                case ImageFormat::DEPTH24STENCIL8:
                case ImageFormat::BC7:
                case ImageFormat::BC5:
                case ImageFormat::BC6H:
                    return 0;
                case ImageFormat::R8:
                case ImageFormat::R8UI:
                    return 1;
                case ImageFormat::R16UI:
                case ImageFormat::RG8:
                    return 2;
                case ImageFormat::RGB8:
                    return 3;
                case ImageFormat::RG16UI:
                case ImageFormat::RGBA8:
                case ImageFormat::R32F:
                case ImageFormat::RG16F:
                case ImageFormat::R32I:
                    return 4;
                case ImageFormat::RGBA16F:
                case ImageFormat::RG32F:
                    return 8;
                case ImageFormat::RGB32F:
                    return 12;
                case ImageFormat::RGBA32F:
                    return 16;
            }
            return 0;
        }

        // Bytes per pixel of the resolved VkFormat's texel (the widened
        // 4-channel form for the 3-channel engine formats).
        [[nodiscard]] u32 VkFormatTexelBytes(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::RGB8:
                    return 4; // widened to RGBA8
                case ImageFormat::RGB32F:
                    return 16; // widened to RGBA32F
                default:
                    return EngineFormatClientBpp(format);
            }
        }

        // Expand tightly-packed 3-channel rows into the widened 4-channel
        // layout the VkImage actually has (opaque alpha).
        [[nodiscard]] std::vector<u8> ExpandRgbToRgba(ImageFormat format, const void* data, u64 pixelCount)
        {
            std::vector<u8> out;
            if (format == ImageFormat::RGB8)
            {
                const auto* src = static_cast<const u8*>(data);
                out.resize(pixelCount * 4);
                for (u64 i = 0; i < pixelCount; ++i)
                {
                    out[i * 4 + 0] = src[i * 3 + 0];
                    out[i * 4 + 1] = src[i * 3 + 1];
                    out[i * 4 + 2] = src[i * 3 + 2];
                    out[i * 4 + 3] = 0xFF;
                }
            }
            else if (format == ImageFormat::RGB32F)
            {
                const auto* src = static_cast<const f32*>(data);
                out.resize(pixelCount * 16);
                auto* dst = reinterpret_cast<f32*>(out.data());
                for (u64 i = 0; i < pixelCount; ++i)
                {
                    dst[i * 4 + 0] = src[i * 3 + 0];
                    dst[i * 4 + 1] = src[i * 3 + 1];
                    dst[i * 4 + 2] = src[i * 3 + 2];
                    dst[i * 4 + 3] = 1.0f;
                }
            }
            return out;
        }

        // One VkImageMemoryBarrier2 over a mip/layer range of a color image —
        // upload-path plumbing (the graph's barriers go through
        // VulkanBarrierLowering, not this). The layer pair defaults to the
        // single-layer shape every 2D upload uses; the cubemap face paths
        // (#691 Phase 8) pass a face index.
        void RecordImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, u32 baseMip, u32 mipCount,
                                u32 baseLayer = 0u, u32 layerCount = 1u)
        {
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, baseLayer, layerCount };

            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1u;
            dep.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
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

    void VulkanImageInfoRegistry::SetInitialLayout(VkImage image, VkImageLayout layout)
    {
        const auto it = m_Infos.find(image);
        if (it != m_Infos.end())
        {
            it->second.InitialLayout = layout;
        }
    }

    void VulkanImageInfoRegistry::SetSamplerFilter(VkImage image, const VkFilter minFilter, const VkFilter magFilter)
    {
        const auto it = m_Infos.find(image);
        if (it != m_Infos.end())
        {
            it->second.MinFilter = minFilter;
            it->second.MagFilter = magFilter;
            // GL couples the mip filter into MIN_FILTER; NEAREST min means
            // no linear mip blend either (the GL_NEAREST /
            // GL_NEAREST_MIPMAP_NEAREST shape callers actually use).
            it->second.MipmapMode =
                minFilter == VK_FILTER_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
    }

    void VulkanImageInfoRegistry::SetSamplerAddressMode(VkImage image, const VkSamplerAddressMode mode)
    {
        const auto it = m_Infos.find(image);
        if (it != m_Infos.end())
        {
            it->second.AddressMode = mode;
        }
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
        m_Entries.push_back({ .Image = image, .Allocation = allocation, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkBuffer buffer, VmaAllocation allocation)
    {
        if (buffer == VK_NULL_HANDLE && allocation == VK_NULL_HANDLE)
        {
            return;
        }
        m_Entries.push_back({ .Buffer = buffer, .Allocation = allocation, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkSemaphore semaphore)
    {
        if (semaphore == VK_NULL_HANDLE)
        {
            return;
        }
        m_Entries.push_back({ .Semaphore = semaphore, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkPipeline pipeline)
    {
        if (pipeline == VK_NULL_HANDLE)
        {
            return;
        }
        m_Entries.push_back({ .Pipeline = pipeline, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkImageView view)
    {
        if (view == VK_NULL_HANDLE)
        {
            return;
        }
        m_Entries.push_back({ .View = view, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::Enqueue(VkQueryPool queryPool)
    {
        if (queryPool == VK_NULL_HANDLE)
        {
            return;
        }
        m_Entries.push_back({ .QueryPool = queryPool, .EnqueuedAtGeneration = m_Generation });
    }

    void VulkanDeferredReclaim::DestroyEntry(const Entry& entry)
    {
        // Image metadata retires at ACTUAL destroy time — a barrier emitted for
        // a still-enqueued image must keep resolving until the image is gone.
        if (entry.Image != VK_NULL_HANDLE)
        {
            VulkanImageInfoRegistry::Get().Unregister(entry.Image);
            // Same reasoning for the layout rows: retiring them any earlier
            // would strand a barrier mid-flight, and never retiring them grows
            // the map for the process lifetime.
            VulkanImageLayoutTracker::ForgetImageEverywhere(entry.Image);
            // And the per-cascade depth views framebuffers cached over this
            // image — they are enqueued here, i.e. strictly BEFORE the
            // vmaDestroyImage below, so no view outlives its image.
            VulkanFramebuffer::ReleaseCachedDepthViewsForImage(entry.Image);
            // Cached heap slots free HERE, not at enqueue: the generation wait
            // this queue already performed is what makes immediate slot reuse
            // safe (no in-flight frame can still index them).
            VulkanDescriptorSlotCache::Get().ReleaseSlotsForImage(entry.Image);
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
        else if (entry.Semaphore != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device->GetDevice(), entry.Semaphore, nullptr);
        }
        else if (entry.Pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device->GetDevice(), entry.Pipeline, nullptr);
        }
        else if (entry.View != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device->GetDevice(), entry.View, nullptr);
        }
        else if (entry.QueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(device->GetDevice(), entry.QueryPool, nullptr);
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

    VulkanTexture2D::VulkanTexture2D(const std::string& path, bool srgb)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(VulkanDevice::Get() != nullptr, "VulkanTexture2D requires a live VulkanDevice");

        int width = 0;
        int height = 0;
        int channels = 0;
        // Thread-local flip, exactly like the GL twin: asset BYTES must be
        // identical across backends (UV sampling is convention-free, so the
        // same bytes give the same result).
        ::stbi_set_flip_vertically_on_load_thread(1);
        stbi_uc* data = nullptr;
        {
            OLO_PROFILE_SCOPE("stbi_load - VulkanTexture2D::VulkanTexture2D(const std::string&)");
            data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
        }
        ::stbi_set_flip_vertically_on_load_thread(0);

        if (data == nullptr)
        {
            OLO_CORE_ERROR("VulkanTexture2D: failed to load image '{}'", path);
            m_Width = 1;
            m_Height = 1;
            m_IsLoaded = false;
            return;
        }

        m_Specification.SRGB = srgb;
        Invalidate(path, static_cast<u32>(width), static_cast<u32>(height), data, static_cast<u32>(channels));
        ::stbi_image_free(data);
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
            // The engine heap's views of this texture retire FIRST — the
            // amendment (22) correction: destruction is RetireResource
            // (poison + generation advance), never InvalidateResource (whose
            // re-acquire is for storage that was replaced, not destroyed).
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
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
                                                             .Width = m_Width,
                                                             .Height = m_Height,
                                                             .MipLevels = m_MipLevels,
                                                             .ArrayLayers = 1u,
                                                             .HasDepth = isDepth,
                                                             .HasStencil = isDepth,
                                                         });

        m_RHIHandle.Sync(RHI::ResourceKind::Texture, VkHandleToU64(m_Image), RHI::Backend::Vulkan);
    }

    void VulkanTexture2D::ReleaseImage()
    {
        if (m_AttachmentView != VK_NULL_HANDLE)
        {
            VulkanDeferredReclaim::Get().Enqueue(m_AttachmentView);
            m_AttachmentView = VK_NULL_HANDLE;
        }
        if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
        {
            // The info-registry entry retires inside the reclaim queue at
            // actual-destroy time, not here.
            VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
        }
    }

    VkImageView VulkanTexture2D::GetOrCreateAttachmentView()
    {
        if (m_AttachmentView != VK_NULL_HANDLE)
        {
            return m_AttachmentView;
        }
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE)
        {
            return VK_NULL_HANDLE;
        }

        const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        if (info == nullptr)
        {
            return VK_NULL_HANDLE;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = info->Format;
        // An attachment view renders into mip 0 / layer 0; a combined
        // depth-stencil format carries BOTH aspects (legal for the depth
        // attachment and required if the stencil half is ever attached).
        viewInfo.subresourceRange.aspectMask =
            info->HasDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (info->HasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u))
                           : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device->GetDevice(), &viewInfo, nullptr, &m_AttachmentView) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2D: attachment view creation failed");
            m_AttachmentView = VK_NULL_HANDLE;
        }
        return m_AttachmentView;
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

        // Storage replaced, object lives: the engine heap's views re-describe
        // against the new image (amendment (22) — a reload must PUSH; the
        // view's generation is unchanged, so OffsetOf cannot detect this).
        RHI::DescriptorHeap::Get().InvalidateResource(m_RHIHandle.Get());
    }

    bool VulkanTexture2D::UploadPixels(const void* data, u64 sizeBytes)
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || data == nullptr || sizeBytes == 0)
        {
            return false;
        }
        if (m_Specification.Samples > 1u)
        {
            OLO_CORE_ERROR("VulkanTexture2D::UploadPixels: multisampled textures have no upload path");
            return false;
        }

        // The VkImage may be the widened 4-channel form of a 3-channel engine
        // format — expand CPU-side when so.
        const void* uploadData = data;
        u64 uploadSize = sizeBytes;
        std::vector<u8> expanded;
        if (m_Specification.Format == ImageFormat::RGB8 || m_Specification.Format == ImageFormat::RGB32F)
        {
            expanded = ExpandRgbToRgba(m_Specification.Format, data, static_cast<u64>(m_Width) * m_Height);
            uploadData = expanded.data();
            uploadSize = expanded.size();
        }

        // Host staging buffer — destroyed right after the blocking one-shot.
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2D::UploadPixels: staging allocation failed ({} bytes)", uploadSize);
            return false;
        }
        std::memcpy(stagingOut.pMappedData, uploadData, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        const bool generateMips = m_MipLevels > 1u;
        const VkFilter blitFilter = IsIntegerFormat(m_Specification.Format) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;

        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture2D::UploadPixels",
            [&](VkCommandBuffer cmd)
            {
                // Whole image (every mip) → TRANSFER_DST. oldLayout UNDEFINED
                // is deliberate: this is a FULL overwrite, discard is free.
                // dst scope includes BLIT: the mip chain's blits write mips
                // this transition covers, and a transition visible only to
                // COPY leaves those writes unordered (sync validation caught
                // exactly this — WAW between the transition and vkCmdBlitImage).
                // src scope is non-empty even though oldLayout is UNDEFINED:
                // SetData re-uploads reach this on an ALREADY-written image
                // (hot reload), and an empty src scope leaves those earlier
                // writes unordered against the transition (the same
                // WRITE_AFTER_WRITE shape sync validation caught on the
                // cubemap chain). Discard semantics are unchanged.
                // src scope is non-empty even though oldLayout is UNDEFINED:
                // SetData re-uploads reach this on an ALREADY-written image
                // (hot reload), and an empty src scope leaves those earlier
                // writes unordered against the transition (the same
                // WRITE_AFTER_WRITE shape sync validation caught on the
                // cubemap chain). Discard semantics are unchanged.
                RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                region.imageExtent = { m_Width, m_Height, 1u };
                vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);

                if (generateMips)
                {
                    // Classic blit chain: mip N-1 (TRANSFER_SRC) → mip N
                    // (TRANSFER_DST), then N becomes the next source.
                    i32 srcW = static_cast<i32>(m_Width);
                    i32 srcH = static_cast<i32>(m_Height);
                    for (u32 mip = 1; mip < m_MipLevels; ++mip)
                    {
                        // src scope: mip 0's last write was the COPY, later
                        // mips' the previous BLIT — cover both rather than
                        // special-casing the first iteration.
                        RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                                           VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                           VK_ACCESS_2_TRANSFER_READ_BIT, mip - 1u, 1u);

                        const i32 dstW = std::max(srcW / 2, 1);
                        const i32 dstH = std::max(srcH / 2, 1);

                        VkImageBlit blit{};
                        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 0u, 1u };
                        blit.srcOffsets[1] = { srcW, srcH, 1 };
                        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0u, 1u };
                        blit.dstOffsets[1] = { dstW, dstH, 1 };
                        vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, blitFilter);

                        srcW = dstW;
                        srcH = dstH;
                    }

                    // Mips [0, N-1) sit in TRANSFER_SRC, the last in
                    // TRANSFER_DST — bring all to SHADER_READ_ONLY.
                    RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels - 1u);
                    RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       VK_ACCESS_2_MEMORY_READ_BIT, m_MipLevels - 1u, 1u);
                }
                else
                {
                    RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels);
                }
            });

        vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);

        if (ok)
        {
            // Seed the layout tracker's first sight of this image (see
            // VulkanImageInfo::InitialLayout) — without this, the graph's
            // first barrier would transition from UNDEFINED and could
            // legally discard the pixels just uploaded.
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return ok;
    }

    void VulkanTexture2D::SetData(void* data, u32 size)
    {
        OLO_PROFILE_FUNCTION();

        const u32 clientBpp = EngineFormatClientBpp(m_Specification.Format);
        if (clientBpp == 0)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SetData: format {} has no client-upload path",
                           static_cast<u32>(m_Specification.Format));
            return;
        }
        const u64 expected = static_cast<u64>(m_Width) * m_Height * clientBpp;
        OLO_CORE_ASSERT(size == expected, "VulkanTexture2D::SetData: size must cover the whole texture");
        if (size != expected)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SetData: got {} bytes, expected {} — dropping the upload", size,
                           expected);
            return;
        }

        m_IsLoaded = UploadPixels(data, size) || m_IsLoaded;
    }

    void VulkanTexture2D::SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize)
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        const u32 clientBpp = EngineFormatClientBpp(m_Specification.Format);
        if (device == nullptr || m_Image == VK_NULL_HANDLE || data == nullptr || clientBpp == 0)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: no upload path (device/image/format)");
            return;
        }
        if (x + width > m_Width || y + height > m_Height)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: region {}x{}+{}+{} exceeds {}x{}", width, height, x, y,
                           m_Width, m_Height);
            return;
        }
        const u64 expected = static_cast<u64>(width) * height * clientBpp;
        if (dataSize < expected)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: got {} bytes, region needs {}", dataSize, expected);
            return;
        }

        // Mid-frame (#691 Phase 8): a region flush between two GPU uses (the
        // terrain sculpt/paint shape) must be ORDERED within the frame
        // command buffer — the one-shot below submits BEFORE the
        // still-recording frame and also diverges from the API's layout
        // tracker. Route through the facade's staged frame-CB upload, which
        // owns both. The one-shot arm below stays for load time (no
        // recording), where it is correct and the tracker learns the layout
        // through InitialLayout.
        // Probed off the LIVE object, not RendererAPI::GetAPI(): a fixture can
        // set the static flag without recreating the process API (amendment
        // (39)'s construction-order gap), and a static_cast through the wrong
        // object was an access violation the first time a device-gated test
        // ran this path.
        if (auto* vk = dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI()); vk != nullptr)
        {
            if (vk->CurrentCommandBuffer() != VK_NULL_HANDLE)
            {
                const RHI::Format clientFormat = [this]
                {
                    switch (m_Specification.Format)
                    {
                        case ImageFormat::R8:
                            return RHI::Format::R8UNorm;
                        case ImageFormat::RGB8:
                            return RHI::Format::RGB8UNorm;
                        case ImageFormat::RGBA8:
                            return RHI::Format::RGBA8UNorm;
                        case ImageFormat::R32F:
                            return RHI::Format::R32Float;
                        case ImageFormat::RG32F:
                            return RHI::Format::RG32Float;
                        case ImageFormat::RGB32F:
                            return RHI::Format::RGB32Float;
                        case ImageFormat::RGBA32F:
                            return RHI::Format::RGBA32Float;
                        default:
                            return RHI::Format::Unknown;
                    }
                }();
                if (clientFormat != RHI::Format::Unknown)
                {
                    vk->UploadTextureSubImage2D(m_RHIHandle.Get(), static_cast<i32>(x), static_cast<i32>(y), width,
                                                height, clientFormat, data);
                    return;
                }
                // An unmapped format falls through to the one-shot with the
                // known previous-frame-ordering caveat — loud, not silent.
                OLO_CORE_WARN("VulkanTexture2D::SubImage: mid-frame upload of unmapped format {} takes the "
                              "one-shot path (ordered BEFORE this frame's GPU work)",
                              static_cast<u32>(m_Specification.Format));
            }
        }

        const void* uploadData = data;
        u64 uploadSize = expected;
        std::vector<u8> expanded;
        if (m_Specification.Format == ImageFormat::RGB8 || m_Specification.Format == ImageFormat::RGB32F)
        {
            expanded = ExpandRgbToRgba(m_Specification.Format, data, static_cast<u64>(width) * height);
            uploadData = expanded.data();
            uploadSize = expanded.size();
        }

        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2D::SubImage: staging allocation failed ({} bytes)", uploadSize);
            return;
        }
        std::memcpy(stagingOut.pMappedData, uploadData, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        // PARTIAL update: the untouched texels must survive, so oldLayout is
        // the steady-state SHADER_READ_ONLY every upload leaves the image in
        // — never UNDEFINED (a legal discard of the rest). Backend invariant:
        // sampled asset textures are not graph-written, so outside graph
        // execution they sit in SHADER_READ_ONLY.
        //
        // Read it back rather than hardcoding it: a texture created but never
        // uploaded is still UNDEFINED, and naming SHADER_READ_ONLY as the
        // oldLayout there is invalid usage (VUID-VkImageMemoryBarrier2-oldLayout-01197).
        // Discarding is harmless in that case — there are no prior texels to keep.
        const auto* imageInfo = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout priorLayout = imageInfo != nullptr ? imageInfo->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        const bool ok = VulkanOneShot::Submit("VulkanTexture2D::SubImage",
                                              [&](VkCommandBuffer cmd)
                                              {
                                                  RecordImageBarrier(cmd, m_Image, priorLayout,
                                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                                                                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u,
                                                                     1u);

                                                  VkBufferImageCopy region{};
                                                  region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
                                                  region.imageOffset = { static_cast<i32>(x), static_cast<i32>(y), 0 };
                                                  region.imageExtent = { width, height, 1u };
                                                  vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                         1u, &region);

                                                  RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT,
                                                                     0u, 1u);
                                              });

        vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
        if (ok)
        {
            // Only on success: recording a layout the image never reached is
            // exactly the wrong-oldLayout hazard InitialLayout exists to stop.
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    void VulkanTexture2D::Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0u || height == 0u || data == nullptr)
        {
            OLO_CORE_ERROR("VulkanTexture2D::Invalidate: bad arguments ({}x{}, data {})", width, height,
                           static_cast<const void*>(data));
            return;
        }

        // Mirror the GL twin's channel handling: 3/4-channel data honours the
        // sRGB flag, 1/2-channel data is linear.
        ImageFormat format;
        switch (channels)
        {
            case 1:
                format = ImageFormat::R8;
                break;
            case 2:
                format = ImageFormat::RG8;
                break;
            case 3:
                format = ImageFormat::RGB8;
                break;
            case 4:
                format = ImageFormat::RGBA8;
                break;
            default:
                OLO_CORE_ERROR("VulkanTexture2D::Invalidate: unsupported channel count {}", channels);
                return;
        }

        ReleaseImage();

        m_Path = std::string(path);
        m_Width = width;
        m_Height = height;
        m_Specification.Width = width;
        m_Specification.Height = height;
        m_Specification.Format = format;
        m_MipLevels = DeriveMipLevels(m_Specification, m_Width, m_Height);

        // Sync inside CreateImage PRESERVES identity — in-place reload, the
        // amendment (12) contract.
        CreateImage();

        const u64 sizeBytes = static_cast<u64>(width) * height * channels;
        m_IsLoaded = UploadPixels(data, sizeBytes);

        // Same contract as Resize: storage replaced, identity preserved —
        // push the re-describe (amendment (22)).
        RHI::DescriptorHeap::Get().InvalidateResource(m_RHIHandle.Get());
    }

    void VulkanTexture2D::Bind(u32 slot) const
    {
        // #691 Phase 8: forward like the 3D/array/cube classes always did —
        // this stub was the one dead end in the family, silently dropping
        // every ShaderResourceRegistry-routed bind (shader-graph materials,
        // ShadowMap's raw views, wind/snow fields, video textures).
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    bool VulkanTexture2D::GetData(std::vector<u8>& outData, u32 mipLevel) const
    {
        OLO_PROFILE_FUNCTION();

        outData.clear();
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || mipLevel >= m_MipLevels)
        {
            return false;
        }
        const u32 texelBytes = VkFormatTexelBytes(m_Specification.Format);
        if (texelBytes == 0 || m_Specification.Samples > 1u)
        {
            return false;
        }

        const u32 mipW = std::max(m_Width >> mipLevel, 1u);
        const u32 mipH = std::max(m_Height >> mipLevel, 1u);
        const u64 sizeBytes = static_cast<u64>(mipW) * mipH * texelBytes;

        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = sizeBytes;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo readbackAlloc{};
        readbackAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        readbackAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo readbackOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &readbackInfo, &readbackAlloc, &readback, &readbackAllocation,
                            &readbackOut) != VK_SUCCESS)
        {
            return false;
        }

        const bool ok = VulkanOneShot::Submit(
            "VulkanTexture2D::GetData",
            [&](VkCommandBuffer cmd)
            {
                // Steady-state invariant again: sampled content sits in
                // SHADER_READ_ONLY between graph executions.
                RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, mipLevel, 1u);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 0u, 1u };
                region.imageExtent = { mipW, mipH, 1u };
                vkCmdCopyImageToBuffer(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);

                RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_ACCESS_2_MEMORY_READ_BIT, mipLevel, 1u);
            });

        if (ok)
        {
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, sizeBytes);
            outData.resize(sizeBytes);
            std::memcpy(outData.data(), readbackOut.pMappedData, sizeBytes);
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
        return ok;
    }

    // =========================================================================
    // VulkanFramebuffer
    // =========================================================================

    // --- VulkanTexture3D -----------------------------------------------------

    namespace
    {
        VkFormat Texture3DFormatToVk(const Texture3DFormat format)
        {
            switch (format)
            {
                case Texture3DFormat::RGBA8:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case Texture3DFormat::RGBA16F:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Texture3DFormat::RGBA32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            return VK_FORMAT_UNDEFINED;
        }
    } // namespace

    VulkanTexture3D::VulkanTexture3D(const Texture3DSpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTexture3D requires a live VulkanDevice");

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.format = Texture3DFormatToVk(spec.Format);
        imageInfo.extent = { std::max(spec.Width, 1u), std::max(spec.Height, 1u), std::max(spec.Depth, 1u) };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Both halves of the volume contract: written as image3D by compute,
        // read as sampler3D by consumers; transfer for clears/debug readback.
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture3D: image creation failed ({}x{}x{})", spec.Width, spec.Height,
                           spec.Depth);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }

        VulkanImageInfo registryInfo{};
        registryInfo.Format = imageInfo.format;
        registryInfo.Width = spec.Width;
        registryInfo.Height = spec.Height;
        registryInfo.MipLevels = 1;
        registryInfo.ArrayLayers = 1;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_3D;
        // §4f sampler table: 3D volumes are caller-supplied on GL; the
        // engine's volumes (froxel fog, noise) all clamp.
        registryInfo.AddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTexture3D::~VulkanTexture3D()
    {
        try
        {
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
            {
                // Slots free in DestroyEntry, NOT here: releasing at enqueue
                // time hands the slot to the next texture while in-flight
                // frames still index it (VulkanDescriptorSlotCache.h's
                // recycling contract). VulkanTexture2D already does it this way.
                VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
                m_Image = VK_NULL_HANDLE;
                m_Allocation = VK_NULL_HANDLE;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanTexture3D dtor: reclaim enqueue threw ({}); the volume leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTexture3D dtor: reclaim enqueue threw; the volume leaks");
        }
    }

    void VulkanTexture3D::Bind(u32 slot) const
    {
        // The facade's slot path — same acquire the handle form performs.
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    namespace
    {
        [[nodiscard]] VkFormat Texture2DArrayFormatToVk(const Texture2DArrayFormat format)
        {
            switch (format)
            {
                case Texture2DArrayFormat::DEPTH_COMPONENT32F:
                    return VK_FORMAT_D32_SFLOAT;
                case Texture2DArrayFormat::RGBA8:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case Texture2DArrayFormat::RGBA16F:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Texture2DArrayFormat::RGBA32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            return VK_FORMAT_UNDEFINED;
        }
    } // namespace

    // --- VulkanTextureCubemap ------------------------------------------------
    namespace
    {
        void WarnCubemapCpuPathOnce(const char* what)
        {
            static std::unordered_set<std::string> s_Warned;
            if (s_Warned.insert(what).second)
            {
                OLO_CORE_WARN("[RHI/Vulkan] TextureCubemap::{} is not implemented (#691 Phase 8: the IBL bake "
                              "path is GPU-side capture work) — no-op",
                              what);
            }
        }
    } // namespace

    VulkanTextureCubemap::VulkanTextureCubemap(const CubemapSpecification& spec)
        : m_CubemapSpecification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTextureCubemap requires a live VulkanDevice");

        const u32 width = std::max(spec.Width, 1u);
        const u32 height = std::max(spec.Height, 1u);
        m_Specification.Width = width;
        m_Specification.Height = height;
        m_Specification.Format = spec.Format;

        // Clamp an authored MipLevels to the chain the extent supports (the
        // DeriveMipLevels rule): an over-large count from a stale IBL cache
        // is a vkCreateImage failure, not a request (#691 Phase 8).
        const u32 fullChain = 1u + static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(width, height)))));
        m_MipLevels = 1u;
        if (spec.MipLevels > 0u)
        {
            m_MipLevels = std::min(spec.MipLevels, fullChain);
        }
        else if (spec.GenerateMips)
        {
            m_MipLevels = fullChain;
        }

        const VkFormat format = ImageFormatToVkFormat(spec.Format, false);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        // The one flag that makes a 6-layer 2D image addressable as a cube.
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = 6u;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap: image creation failed ({}x{}, {} mips)", width, height, m_MipLevels);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }

        VulkanImageInfo registryInfo{};
        registryInfo.Format = format;
        registryInfo.Width = width;
        registryInfo.Height = height;
        registryInfo.MipLevels = m_MipLevels;
        registryInfo.ArrayLayers = 6u;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_CUBE;
        // §4f sampler table: cubemaps are CLAMP_TO_EDGE on GL.
        registryInfo.AddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTextureCubemap::~VulkanTextureCubemap()
    {
        try
        {
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
            {
                // Slots free in DestroyEntry, NOT here: releasing at enqueue
                // time hands the slot to the next texture while in-flight
                // frames still index it (VulkanDescriptorSlotCache.h's
                // recycling contract). VulkanTexture2D already does it this way.
                VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
                m_Image = VK_NULL_HANDLE;
                m_Allocation = VK_NULL_HANDLE;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap dtor: reclaim enqueue threw ({}); the cubemap leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap dtor: reclaim enqueue threw; the cubemap leaks");
        }
    }

    void VulkanTextureCubemap::Bind(u32 slot) const
    {
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTextureCubemap::SetData(void* /*data*/, u32 /*size*/)
    {
        WarnCubemapCpuPathOnce("SetData");
    }

    void VulkanTextureCubemap::Invalidate(std::string_view /*path*/, u32 /*width*/, u32 /*height*/,
                                          const void* /*data*/, u32 /*channels*/)
    {
        WarnCubemapCpuPathOnce("Invalidate");
    }

    void VulkanTextureCubemap::SetFaceData(u32 faceIndex, void* data, u32 size)
    {
        // GL contract (OpenGLTextureCubemap::SetFaceData): mip-0 face upload
        // plus a full mip regeneration when the spec asks for mips.
        if (!SetFaceDataMip(faceIndex, 0u, data, size))
        {
            return;
        }
        if (m_CubemapSpecification.GenerateMips && m_MipLevels > 1u)
        {
            GenerateMipmaps();
        }
    }

    bool VulkanTextureCubemap::SetFaceDataMip(u32 faceIndex, u32 mipLevel, void* data, u32 size)
    {
        // #691 Phase 8: the cubemap CPU face upload — six layers of the
        // VulkanTexture2D staging shape. This is the IBL cache's load path
        // and the reflection-probe baker's face write, i.e. most of what
        // stood between the flat grey sky and a lit environment.
        auto* device = VulkanDevice::Get();
        const u32 clientBpp = EngineFormatClientBpp(m_CubemapSpecification.Format);
        if (device == nullptr || m_Image == VK_NULL_HANDLE || data == nullptr || clientBpp == 0u || faceIndex >= 6u ||
            mipLevel >= m_MipLevels)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap::SetFaceDataMip: no upload path (face {}, mip {}/{})", faceIndex,
                           mipLevel, m_MipLevels);
            return false;
        }
        const u32 mipWidth = std::max(m_CubemapSpecification.Width >> mipLevel, 1u);
        const u32 mipHeight = std::max(m_CubemapSpecification.Height >> mipLevel, 1u);
        const u64 expected = static_cast<u64>(mipWidth) * mipHeight * clientBpp;
        if (size < expected)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap::SetFaceDataMip: got {} bytes, face mip needs {}", size, expected);
            return false;
        }

        const void* uploadData = data;
        u64 uploadSize = expected;
        std::vector<u8> expanded;
        if (m_CubemapSpecification.Format == ImageFormat::RGB8 || m_CubemapSpecification.Format == ImageFormat::RGB32F)
        {
            expanded = ExpandRgbToRgba(m_CubemapSpecification.Format, data, static_cast<u64>(mipWidth) * mipHeight);
            uploadData = expanded.data();
            uploadSize = expanded.size();
        }

        // Mid-frame (the IBL cache load runs inside the frame on this
        // backend): record into the FRAME command buffer through the API's
        // tracker — a one-shot here would submit BEFORE the frame and race
        // the layout tracking (the 1c ordering rule).
        if (auto* vk = dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI());
            vk != nullptr && vk->CurrentCommandBuffer() != VK_NULL_HANDLE)
        {
            return vk->RecordStagedImageUpload(m_Image, mipLevel, faceIndex, mipWidth, mipHeight, uploadData,
                                               uploadSize);
        }

        // Load time (no recording): the blocking one-shot, the
        // VulkanTexture2D::UploadPixels shape with the face as the layer.
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingOut) != VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTextureCubemap::SetFaceDataMip: staging allocation failed ({} bytes)", uploadSize);
            return false;
        }
        std::memcpy(stagingOut.pMappedData, uploadData, uploadSize);
        vmaFlushAllocation(device->GetAllocator(), stagingAllocation, 0, uploadSize);

        const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout priorLayout = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        const bool ok = VulkanOneShot::Submit(
            "VulkanTextureCubemap::SetFaceDataMip",
            [&](VkCommandBuffer cmd)
            {
                // Whole image through the transition, not just this face:
                // partial-face-uploaded cubes must keep a UNIFORM tracked
                // layout or CurrentLayout answers UNDEFINED (a legal discard)
                // for whole-image queries — the mixed-layout trap.
                RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   priorLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE
                                                                            : VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels, 0u,
                                   6u);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, faceIndex, 1u };
                region.imageExtent = { mipWidth, mipHeight, 1u };
                vkCmdCopyBufferToImage(cmd, staging, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
                RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels, 0u, 6u);
            });
        vmaDestroyBuffer(device->GetAllocator(), staging, stagingAllocation);
        if (ok)
        {
            VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return ok;
    }

    void VulkanTextureCubemap::GenerateMipmaps() const
    {
        // Mip chain by blit, all six faces per level in ONE blit (the
        // subresource layerCount carries the fan-out). Runs in the frame
        // command buffer when one is live (transfer ops are legal outside the
        // rendering scope), else as a one-shot.
        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Image == VK_NULL_HANDLE || m_MipLevels <= 1u)
        {
            return;
        }

        const auto recordChain = [this](VkCommandBuffer cmd, const VkImageLayout priorLayout)
        {
            // src access stays MEMORY_WRITE even when priorLayout is
            // UNDEFINED: for this image UNDEFINED can mean "mixed after
            // per-face copies" (SetFaceData collapses a partially-uploaded
            // layout to UNDEFINED), so the face uploads' TRANSFER_WRITEs may
            // still be in flight — an empty src scope is the WRITE_AFTER_WRITE
            // sync-validation hazard the live editor hit. A transition FROM
            // UNDEFINED with a non-empty src scope is legal; it only orders
            // the prior writes, the contents are discarded either way.
            // src access stays MEMORY_WRITE even when priorLayout is
            // UNDEFINED: for this image UNDEFINED can mean "mixed after
            // per-face copies" (SetFaceData collapses a partially-uploaded
            // layout to UNDEFINED), so the face uploads' TRANSFER_WRITEs may
            // still be in flight — an empty src scope is the WRITE_AFTER_WRITE
            // sync-validation hazard the live editor hit. A transition FROM
            // UNDEFINED with a non-empty src scope is legal; it only orders
            // the prior writes, the contents are discarded either way.
            RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, 0u, m_MipLevels, 0u,
                               6u);
            u32 srcWidth = m_CubemapSpecification.Width;
            u32 srcHeight = m_CubemapSpecification.Height;
            for (u32 mip = 1u; mip < m_MipLevels; ++mip)
            {
                RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_READ_BIT, mip - 1u, 1u, 0u, 6u);
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1u, 0u, 6u };
                blit.srcOffsets[1] = { static_cast<i32>(std::max(srcWidth, 1u)),
                                       static_cast<i32>(std::max(srcHeight, 1u)), 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 0u, 6u };
                blit.dstOffsets[1] = { static_cast<i32>(std::max(srcWidth >> 1u, 1u)),
                                       static_cast<i32>(std::max(srcHeight >> 1u, 1u)), 1 };
                vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_Image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit, VK_FILTER_LINEAR);
                srcWidth = std::max(srcWidth >> 1u, 1u);
                srcHeight = std::max(srcHeight >> 1u, 1u);
            }
            // Unify: mips [0, N-1) sit in TRANSFER_SRC, the last in DST.
            RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels - 1u, 0u, 6u);
            RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_MEMORY_READ_BIT, m_MipLevels - 1u, 1u, 0u, 6u);
        };

        if (auto* vk = dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI());
            vk != nullptr && vk->CurrentCommandBuffer() != VK_NULL_HANDLE)
        {
            // In-frame: the tracker must agree with the chain's transitions.
            // The chain works in whole-subresource strokes, so drive it with
            // the tracker's whole-image answer and settle everything to
            // SHADER_READ_ONLY afterwards.
            auto& tracker = vk->LayoutTracker();
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
            tracker.RegisterImage(m_Image, m_MipLevels, 6u, info != nullptr ? info->RegistrationId : 0u,
                                  info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED);
            const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, m_MipLevels, 0u, 6u };
            const VkImageLayout prior = tracker.CurrentLayout(m_Image, whole);
            recordChain(vk->CurrentCommandBuffer(), prior);
            tracker.SetLayout(m_Image, whole, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        else
        {
            const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
            const VkImageLayout prior = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
            (void)VulkanOneShot::Submit("VulkanTextureCubemap::GenerateMipmaps",
                                        [&](VkCommandBuffer cmd)
                                        { recordChain(cmd, prior); });
        }
        VulkanImageInfoRegistry::Get().SetInitialLayout(m_Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    bool VulkanTextureCubemap::GetFaceData(u32 faceIndex, std::vector<u8>& outData, u32 mipLevel) const
    {
        outData.clear();
        auto* device = VulkanDevice::Get();
        const u32 clientBpp = EngineFormatClientBpp(m_CubemapSpecification.Format);
        if (device == nullptr || m_Image == VK_NULL_HANDLE || clientBpp == 0u || faceIndex >= 6u ||
            mipLevel >= m_MipLevels)
        {
            return false;
        }
        // Mid-frame: the face's content may still sit unsubmitted in the
        // frame command buffer — the StorageBuffer::GetData rule. Flush; a
        // refusal falls back to previous-frame data with the 1c warn-once.
        if (auto* vk = dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI());
            vk != nullptr && vk->CurrentCommandBuffer() != VK_NULL_HANDLE)
        {
            auto* context = VulkanContext::Get();
            if (context == nullptr || !context->FlushFrameRecordingAndWait())
            {
                static bool s_Warned = false;
                if (!s_Warned)
                {
                    s_Warned = true;
                    OLO_CORE_WARN("[Vulkan] mid-frame cubemap GetFaceData without a frame flush — the readback "
                                  "may return stale contents");
                }
            }
        }

        const u32 mipWidth = std::max(m_CubemapSpecification.Width >> mipLevel, 1u);
        const u32 mipHeight = std::max(m_CubemapSpecification.Height >> mipLevel, 1u);
        // The engine face format is what GL hands back (RGB stays RGB); the
        // backend stores RGB widened to RGBA, so read RGBA and narrow.
        const bool widened =
            m_CubemapSpecification.Format == ImageFormat::RGB8 || m_CubemapSpecification.Format == ImageFormat::RGB32F;
        const u32 storedBpp = widened ? (clientBpp / 3u) * 4u : clientBpp;
        const u64 storedSize = static_cast<u64>(mipWidth) * mipHeight * storedBpp;

        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = storedSize;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo readbackAlloc{};
        readbackAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        readbackAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo readbackOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &readbackInfo, &readbackAlloc, &readback, &readbackAllocation,
                            &readbackOut) != VK_SUCCESS)
        {
            return false;
        }

        const auto* info = VulkanImageInfoRegistry::Get().Lookup(m_Image);
        const VkImageLayout priorLayout = info != nullptr ? info->InitialLayout : VK_IMAGE_LAYOUT_UNDEFINED;
        if (priorLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            // Nothing was ever uploaded/rendered — a read would be garbage.
            vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
            return false;
        }
        const bool ok = VulkanOneShot::Submit(
            "VulkanTextureCubemap::GetFaceData",
            [&](VkCommandBuffer cmd)
            {
                RecordImageBarrier(cmd, m_Image, priorLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0u, m_MipLevels, 0u,
                                   6u);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, faceIndex, 1u };
                region.imageExtent = { mipWidth, mipHeight, 1u };
                vkCmdCopyImageToBuffer(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1u, &region);
                RecordImageBarrier(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, priorLayout,
                                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT, 0u, m_MipLevels,
                                   0u, 6u);
            });
        if (ok)
        {
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, storedSize);
            const auto* stored = static_cast<const u8*>(readbackOut.pMappedData);
            if (widened)
            {
                // Narrow RGBA back to the RGB the caller's format promises.
                const u64 texels = static_cast<u64>(mipWidth) * mipHeight;
                const u32 channelBytes = clientBpp / 3u;
                outData.resize(texels * clientBpp);
                for (u64 i = 0; i < texels; ++i)
                {
                    std::memcpy(outData.data() + i * clientBpp, stored + i * storedBpp,
                                static_cast<sizet>(clientBpp));
                }
            }
            else
            {
                outData.assign(stored, stored + storedSize);
            }
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
        return ok;
    }

    bool VulkanTextureCubemap::GetData(std::vector<u8>& outData, u32 mipLevel) const
    {
        // GL contract (OpenGLTextureCubemap::GetData): all six faces
        // contiguous in face order.
        outData.clear();
        std::vector<u8> face;
        for (u32 i = 0; i < 6u; ++i)
        {
            if (!GetFaceData(i, face, mipLevel))
            {
                outData.clear();
                return false;
            }
            outData.insert(outData.end(), face.begin(), face.end());
        }
        return true;
    }

    // --- VulkanTextureCubemapArray (#691 Phase 8) ----------------------------
    namespace
    {
        void WarnCubemapArrayCpuPathOnce(const char* what)
        {
            static std::unordered_set<std::string> s_Warned;
            if (s_Warned.insert(what).second)
            {
                OLO_CORE_WARN("[RHI/Vulkan] TextureCubemapArray::{} is not implemented (#691 Phase 8: the probe "
                              "bake fill path is capture work) — no-op",
                              what);
            }
        }
    } // namespace

    VulkanTextureCubemapArray::VulkanTextureCubemapArray(const CubemapArraySpecification& spec)
        : m_ArraySpecification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTextureCubemapArray requires a live VulkanDevice");

        const u32 resolution = std::max(spec.Resolution, 1u);
        const u32 layers = std::max(spec.Layers, 1u);
        m_Specification.Width = resolution;
        m_Specification.Height = resolution;
        m_Specification.Format = spec.Format;

        m_MipLevels = spec.MipLevels > 0u
                          ? spec.MipLevels
                          : 1u + static_cast<u32>(std::floor(std::log2(static_cast<f64>(resolution))));

        const VkFormat format = ImageFormatToVkFormat(spec.Format, false);

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        // CUBE_COMPATIBLE + 6*N layers is what makes the image addressable as
        // samplerCubeArray (the view type carries the rest).
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { resolution, resolution, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = 6u * layers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray: image creation failed ({}x{}, {} layers, {} mips)", resolution,
                           resolution, layers, m_MipLevels);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }

        VulkanImageInfo registryInfo{};
        registryInfo.Format = format;
        registryInfo.Width = resolution;
        registryInfo.Height = resolution;
        registryInfo.MipLevels = m_MipLevels;
        registryInfo.ArrayLayers = 6u * layers;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        // §4f sampler table: cubemaps are CLAMP_TO_EDGE on GL — the array
        // flavour follows its element type.
        registryInfo.AddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTextureCubemapArray::~VulkanTextureCubemapArray()
    {
        try
        {
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
            {
                // Slots free in DestroyEntry, not here — the
                // VulkanDescriptorSlotCache recycling contract.
                VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
                m_Image = VK_NULL_HANDLE;
                m_Allocation = VK_NULL_HANDLE;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray dtor: reclaim enqueue threw ({}); the array leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTextureCubemapArray dtor: reclaim enqueue threw; the array leaks");
        }
    }

    void VulkanTextureCubemapArray::Bind(u32 slot) const
    {
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTextureCubemapArray::SetData(void* /*data*/, u32 /*size*/)
    {
        WarnCubemapArrayCpuPathOnce("SetData");
    }

    void VulkanTextureCubemapArray::Invalidate(std::string_view /*path*/, u32 /*width*/, u32 /*height*/,
                                               const void* /*data*/, u32 /*channels*/)
    {
        WarnCubemapArrayCpuPathOnce("Invalidate");
    }

    bool VulkanTextureCubemapArray::SetLayerMipData(u32 /*layer*/, u32 /*mip*/, const void* /*data*/,
                                                    sizet /*sizeBytes*/)
    {
        WarnCubemapArrayCpuPathOnce("SetLayerMipData");
        return false;
    }

    bool VulkanTextureCubemapArray::CopyLayerFromCubemap(u32 /*layer*/, const TextureCubemap& /*source*/)
    {
        WarnCubemapArrayCpuPathOnce("CopyLayerFromCubemap");
        return false;
    }

    bool VulkanTextureCubemapArray::GetData(std::vector<u8>& outData, u32 /*mipLevel*/) const
    {
        outData.clear();
        WarnCubemapArrayCpuPathOnce("GetData");
        return false;
    }

    VulkanTexture2DArray::VulkanTexture2DArray(const Texture2DArraySpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();
        auto* device = VulkanDevice::Get();
        OLO_CORE_ASSERT(device != nullptr, "VulkanTexture2DArray requires a live VulkanDevice");

        const VkFormat format = Texture2DArrayFormatToVk(spec.Format);
        const bool isDepth = spec.Format == Texture2DArrayFormat::DEPTH_COMPONENT32F;
        const u32 width = std::max(spec.Width, 1u);
        const u32 height = std::max(spec.Height, 1u);
        m_MipLevels = 1u;
        if (spec.GenerateMipmaps)
        {
            m_MipLevels = 1u + static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(width, height)))));
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1u };
        imageInfo.mipLevels = m_MipLevels;
        imageInfo.arrayLayers = std::max(spec.Layers, 1u);
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Sampled everywhere (shadow arrays feed sampler2DArrayShadow);
        // depth formats render as layered depth attachments (the CSM/atlas
        // passes), colour formats copy in/out for uploads and debug reads.
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          (isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device->GetAllocator(), &imageInfo, &allocInfo, &m_Image, &m_Allocation, nullptr) !=
            VK_SUCCESS)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray: image creation failed ({}x{}x{} layers)", spec.Width, spec.Height,
                           spec.Layers);
            m_Image = VK_NULL_HANDLE;
            m_Allocation = VK_NULL_HANDLE;
            return;
        }

        VulkanImageInfo registryInfo{};
        registryInfo.Format = imageInfo.format;
        registryInfo.Width = spec.Width;
        registryInfo.Height = spec.Height;
        registryInfo.MipLevels = m_MipLevels;
        registryInfo.ArrayLayers = imageInfo.arrayLayers;
        registryInfo.HasDepth = isDepth;
        registryInfo.ViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        // §4f sampler table: colour arrays are CLAMP_TO_EDGE on GL, depth
        // arrays CLAMP_TO_BORDER with an opaque-white border (the
        // out-of-cascade shadow lookup must read "fully lit").
        registryInfo.AddressMode =
            isDepth ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VulkanImageInfoRegistry::Get().Register(m_Image, registryInfo);

        m_RHIHandle.Adopt(RHI::ResourceKind::Texture, reinterpret_cast<u64>(m_Image), RHI::Backend::Vulkan);
    }

    VulkanTexture2DArray::~VulkanTexture2DArray()
    {
        try
        {
            RHI::DescriptorHeap::Get().RetireResource(m_RHIHandle.Get());
            m_RHIHandle.Reset();
            if (m_Image != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE)
            {
                // Slots free in DestroyEntry, NOT here: releasing at enqueue
                // time hands the slot to the next texture while in-flight
                // frames still index it (VulkanDescriptorSlotCache.h's
                // recycling contract). VulkanTexture2D already does it this way.
                VulkanDeferredReclaim::Get().Enqueue(m_Image, m_Allocation);
                m_Image = VK_NULL_HANDLE;
                m_Allocation = VK_NULL_HANDLE;
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray dtor: reclaim enqueue threw ({}); the array leaks", e.what());
        }
        catch (...)
        {
            OLO_CORE_ERROR("VulkanTexture2DArray dtor: reclaim enqueue threw; the array leaks");
        }
    }

    void VulkanTexture2DArray::Bind(u32 slot) const
    {
        // The facade's slot path — same acquire the handle form performs.
        RenderCommand::GetRendererAPI().BindTexture(slot, m_RHIHandle.Get());
    }

    void VulkanTexture2DArray::SetLayerData(u32 /*layer*/, const void* /*data*/, u32 /*width*/, u32 /*height*/)
    {
        static bool s_Warned = false;
        if (!s_Warned)
        {
            s_Warned = true;
            OLO_CORE_WARN("[RHI/Vulkan] Texture2DArray::SetLayerData is a Wave C concern (#691) — no-op");
        }
    }

    void VulkanTexture2DArray::GenerateMipmaps()
    {
        static bool s_Warned = false;
        if (!s_Warned)
        {
            s_Warned = true;
            OLO_CORE_WARN("[RHI/Vulkan] Texture2DArray::GenerateMipmaps is a Wave C concern (#691) — no-op");
        }
    }

    namespace
    {
        // Every live VulkanFramebuffer, so the reclaim pass can reach the
        // per-cascade depth views they cache over EXTERNAL array images.
        // Deliberately leaked, same rationale as this backend's other
        // process-wide registries (a framebuffer released during static
        // teardown must still find a live set).
        std::unordered_set<VulkanFramebuffer*>& LiveFramebuffers()
        {
            static auto* s_Live = new std::unordered_set<VulkanFramebuffer*>();
            return *s_Live;
        }
    } // namespace

    void VulkanFramebuffer::ReleaseCachedDepthViewsForImage(const VkImage image)
    {
        if (image == VK_NULL_HANDLE)
            return;
        auto* device = VulkanDevice::Get();
        for (auto* framebuffer : LiveFramebuffers())
        {
            auto& cache = framebuffer->m_DepthArrayViews;
            for (auto it = cache.begin(); it != cache.end();)
            {
                if (it->second.SourceImage != image)
                {
                    ++it;
                    continue;
                }
                // Destroyed INLINE, not enqueued. This runs from
                // VulkanDeferredReclaim::DestroyEntry, which is itself inside
                // an erase_if over the queue's own entry vector — enqueueing
                // here would reallocate the container being iterated. Inline is
                // also simply correct: the queue has already waited
                // kFramesInFlight generations past this image's last use, so no
                // in-flight frame can still reference a view of it.
                if (it->second.View != VK_NULL_HANDLE && device != nullptr)
                    vkDestroyImageView(device->GetDevice(), it->second.View, nullptr);
                // The current selection may name the view being retired; drop
                // it so a later scope cannot attach a destroyed view.
                if (framebuffer->m_DepthArrayAttachment.View == it->second.View)
                    framebuffer->m_DepthArrayAttachment = DepthArrayLayerAttachment{};
                it = cache.erase(it);
            }
        }
    }

    VulkanFramebuffer::VulkanFramebuffer(const FramebufferSpecification& spec)
        : m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();
        LiveFramebuffers().insert(this);
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
        // Raw-handle framebuffer ops (ClearFramebuffer* / BlitFramebuffer /
        // the per-FB draw-attachment selection, #691 Phase 7 Wave C) receive
        // only this handle and need the OBJECT back — the native is 0, so the
        // root-object side table is the resolve path, exactly as for VAOs.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::Framebuffer, this);
    }

    VulkanFramebuffer::~VulkanFramebuffer()
    {
        VulkanBindingState::Get().ClearIfCurrentFramebuffer(this);
        VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
        // The per-cascade depth views (AttachDepthTextureArrayLayer) are owned
        // here, not by the array texture — they outlive no frame of their own,
        // so they retire on the deferred queue like every other view.
        for (const auto& [key, cached] : m_DepthArrayViews)
        {
            if (cached.View != VK_NULL_HANDLE)
                VulkanDeferredReclaim::Get().Enqueue(cached.View);
        }
        m_DepthArrayViews.clear();
        LiveFramebuffers().erase(this);
        m_DepthArrayAttachment = DepthArrayLayerAttachment{};
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

        // GL's PrepareTexture (OpenGLUtilities.cpp) stamps every framebuffer
        // attachment CLAMP_TO_EDGE + LINEAR — different from a plain
        // OpenGLTexture2D's REPEAT — and the inherit sampler path (#691
        // Phase 8) reproduces whatever the creator stamped. Without this, a
        // post-process read past uv 1.0 wraps to the far side of the frame
        // (the chromatic-aberration tenant's white edge sampled the black
        // left border the moment inherit landed).
        const auto stampAttachmentSamplerState = [](const Ref<VulkanTexture2D>& attachment)
        {
            auto& registry = VulkanImageInfoRegistry::Get();
            registry.SetSamplerFilter(attachment->GetVkImage(), VK_FILTER_LINEAR, VK_FILTER_LINEAR);
            registry.SetSamplerAddressMode(attachment->GetVkImage(), VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        };

        m_ColorAttachments.reserve(m_ColorAttachmentSpecifications.size());
        for (const auto& attachmentSpec : m_ColorAttachmentSpecifications)
        {
            m_ColorAttachments.push_back(Ref<VulkanTexture2D>::Create(makeAttachmentSpec(attachmentSpec.TextureFormat)));
            stampAttachmentSamplerState(m_ColorAttachments.back());
        }

        if (m_DepthAttachmentSpecification.TextureFormat != FramebufferTextureFormat::None)
        {
            m_DepthAttachment = Ref<VulkanTexture2D>::Create(makeAttachmentSpec(m_DepthAttachmentSpecification.TextureFormat));
            stampAttachmentSamplerState(m_DepthAttachment);
        }
    }

    void VulkanFramebuffer::Bind()
    {
        // Publish as the current render target. The dynamic-rendering scope
        // (VulkanRendererAPI) consumes this LAZILY at the next draw/clear —
        // nothing is recorded here, matching how GL passes freely interleave
        // binds with state calls.
        VulkanBindingState::Get().SetCurrentFramebuffer(this);
    }

    void VulkanFramebuffer::Unbind()
    {
        auto& state = VulkanBindingState::Get();
        if (state.GetCurrentFramebuffer() == this)
        {
            state.SetCurrentFramebuffer(nullptr);
        }
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
        // GL-parity semantics (OpenGLFramebuffer::ClearAllAttachments): every
        // float colour attachment clears to `clearColor`, every RED_INTEGER
        // attachment to `entityIdClear`, the depth attachment to 1.0 (stencil
        // 0). Routed through the facade's transfer clears — each resolves the
        // attachment via the registries, ends the rendering scope first, and
        // issues exact per-layout-run transitions through the layout tracker
        // (the ClearTextureFloat/UInt shape), so the tracker stays true for
        // whatever samples or renders these attachments next (#691 Phase 7
        // Wave A — UICompositePass's mixed int/float clear is the first
        // caller on this backend).
        for (sizet i = 0; i < m_ColorAttachmentSpecifications.size() && i < m_ColorAttachments.size(); ++i)
        {
            if (!m_ColorAttachments[i])
                continue;
            const RHI::ResourceHandle attachment = m_ColorAttachments[i]->GetRHIHandle();
            if (m_ColorAttachmentSpecifications[i].TextureFormat == FramebufferTextureFormat::RED_INTEGER)
            {
                // vkCmdClearColorImage on an SINT image reads the int32 union
                // lanes; the uint clear writes the same bit pattern, so the
                // cast is bit-exact for the -1 sentinel and every other id.
                RenderCommand::ClearTextureUInt(attachment, 0u, static_cast<u32>(entityIdClear));
            }
            else
            {
                RenderCommand::ClearTextureFloat(attachment, 0u, clearColor);
            }
        }

        if (m_DepthAttachment)
        {
            // ClearTextureFloat's depth path clears depth = color.r, stencil 0
            // — the GL twin's glClearDepth(1.0) / glClearStencil(0) pair.
            RenderCommand::ClearTextureFloat(m_DepthAttachment->GetRHIHandle(), 0u,
                                             glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        }
    }

    RHI::ResourceHandle VulkanFramebuffer::GetColorAttachmentHandle(u32 index) const
    {
        OLO_CORE_ASSERT(index < m_ColorAttachments.size());
        if (index >= m_ColorAttachments.size())
        {
            return {};
        }
        // A null slot is legal on raw framebuffers (a detached or never-
        // attached index below a higher attached one) — "no identity", not a
        // crash.
        if (m_ColorAttachments[index] == nullptr)
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
        OLO_PROFILE_FUNCTION();

        // See the DepthArrayLayerAttachment comment in the header: this does
        // not RE-POINT anything (there is no VkFramebuffer under dynamic
        // rendering) — it selects the single-layer depth view the NEXT
        // rendering scope opens against. The scope currently open on this
        // framebuffer still holds the PREVIOUS layer's view; ending it is the
        // backend's business, not this setter's, and VulkanRendererAPI does it
        // by comparing the live scope's recorded selection against this one
        // (ScopeMatchesCurrentTarget) — the same shape as the "the bound
        // framebuffer changed" guard in Clear()/ClearDepthOnly(). Without that
        // comparison every cascade would render into cascade 0's view, which
        // is exactly the failure the layered-depth tenant pins.
        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: no live device");
            return;
        }

        const u64 native = RHI::ResourceRegistry::Get().ResolveNativeForBackend(textureArray);
        if (native == 0)
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: unresolvable texture-array handle");
            m_DepthArrayAttachment = DepthArrayLayerAttachment{};
            return;
        }
        const auto image = reinterpret_cast<VkImage>(native);
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(image);
        if (info == nullptr)
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: image not in the info registry");
            m_DepthArrayAttachment = DepthArrayLayerAttachment{};
            return;
        }
        if (layer >= std::max(info->ArrayLayers, 1u))
        {
            OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: layer {} >= arrayLayers {}", layer,
                           info->ArrayLayers);
            m_DepthArrayAttachment = DepthArrayLayerAttachment{};
            return;
        }

        const u64 cacheKey = VkHandleToU64(image) ^ (static_cast<u64>(layer + 1u) << 48u);
        VkImageView view = VK_NULL_HANDLE;
        if (const auto it = m_DepthArrayViews.find(cacheKey); it != m_DepthArrayViews.end())
        {
            view = it->second.View;
        }
        else
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = image;
            // A single-layer view of an array image: 2D, not 2D_ARRAY — a
            // depth attachment renders into exactly one layer and the
            // rendering info's layerCount stays 1 (no multiview here).
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = info->Format;
            viewInfo.subresourceRange.aspectMask =
                info->HasDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (info->HasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u))
                               : VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = layer;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(device->GetDevice(), &viewInfo, nullptr, &view) != VK_SUCCESS)
            {
                OLO_CORE_ERROR("VulkanFramebuffer::AttachDepthTextureArrayLayer: view creation failed (layer {})",
                               layer);
                m_DepthArrayAttachment = DepthArrayLayerAttachment{};
                return;
            }
            m_DepthArrayViews.emplace(cacheKey, CachedDepthArrayView{ .View = view, .SourceImage = image });
        }

        m_DepthArrayAttachment.Image = image;
        m_DepthArrayAttachment.View = view;
        m_DepthArrayAttachment.Format = info->Format;
        m_DepthArrayAttachment.Handle = textureArray;
        m_DepthArrayAttachment.Layer = layer;
        m_DepthArrayAttachment.Active = true;
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

    void VulkanFramebuffer::AdoptExternalExtent(const VulkanTexture2D& texture)
    {
        // Raw callers attach same-sized textures (the GL completeness rule),
        // so the last non-null attach legitimately owns the spec extent.
        m_Specification.Width = texture.GetWidth();
        m_Specification.Height = texture.GetHeight();
    }

    void VulkanFramebuffer::AttachExternalColorTexture(u32 index, Ref<VulkanTexture2D> texture)
    {
        if (texture == nullptr)
        {
            // Detach (GL's texture-0 form). An index that was never attached
            // needs no slot minted for it.
            if (index < m_ColorAttachments.size())
            {
                m_ColorAttachments[index] = nullptr;
            }
            return;
        }
        if (index >= m_ColorAttachments.size())
        {
            // Growth leaves null gaps below `index` — the scope's
            // VK_ATTACHMENT_UNUSED shape, and IsFramebufferComplete only
            // requires the NON-null attachments to be live.
            m_ColorAttachments.resize(static_cast<sizet>(index) + 1u);
        }
        AdoptExternalExtent(*texture);
        m_ColorAttachments[index] = std::move(texture);
    }

    void VulkanFramebuffer::AttachExternalDepthTexture(Ref<VulkanTexture2D> texture)
    {
        if (texture == nullptr)
        {
            m_DepthAttachment = nullptr;
            return;
        }
        // The rendering scope opens this attachment DEPTH_STENCIL_OPTIMAL
        // with a depth-aspect view — a color-format image there is a
        // validation error, so refuse it here where the mistake is nameable.
        const auto* info = VulkanImageInfoRegistry::Get().Lookup(texture->GetVkImage());
        if (info == nullptr || !info->HasDepth)
        {
            OLO_CORE_WARN("[RHI/Vulkan] AttachExternalDepthTexture: texture has no depth aspect — attach refused");
            return;
        }
        AdoptExternalExtent(*texture);
        m_DepthAttachment = std::move(texture);
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
            VulkanBindingState::Get().ClearBuffer(this);
            VulkanRootObjectRegistry::Get().Unregister(m_RHIHandle.Get());
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
        // SHADER_DEVICE_ADDRESS: Phase 6's root-data model (ADR 0011 §4)
        // addresses buffers by VkDeviceAddress embedded in the root struct, so
        // every storage buffer must be addressable. bufferDeviceAddress is
        // enabled at device creation and the VMA allocator carries
        // VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT.
        // INDIRECT_BUFFER: several StorageBuffer tenants double as indirect
        // argument sources (the ShaderDebugDraw channels ARE their own
        // DrawArraysIndirect args; the virtual-geometry command/args buffers
        // feed vkCmdDrawIndexedIndirectCount; GPU particles' indirect-draw
        // SSBO) — #691 Phase 7 Wave C. Costs nothing on buffers never drawn
        // from.
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        // DynamicDraw means "CPU writes, GPU reads": let VMA prefer a
        // host-writable (BAR/host-visible) placement when one exists, falling
        // back to device-local + a transfer path otherwise. DynamicCopy is
        // GPU-writes/GPU-reads and stays pure device-local.
        if (m_Usage == StorageBufferUsage::DynamicDraw)
        {
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        VmaAllocationInfo outInfo{};
        VkCheck(vmaCreateBuffer(device->GetAllocator(), &bufferInfo, &allocInfo, &m_Buffer, &m_Allocation, &outInfo),
                "vmaCreateBuffer (VulkanStorageBuffer)");

        m_Mapped = nullptr;
        m_NeedsFlush = false;
        VkMemoryPropertyFlags memProps = 0;
        vmaGetAllocationMemoryProperties(device->GetAllocator(), m_Allocation, &memProps);
        if ((memProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            m_Mapped = outInfo.pMappedData;
            m_NeedsFlush = (memProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0;
        }

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = m_Buffer;
        m_DeviceAddress = vkGetBufferDeviceAddress(device->GetDevice(), &addressInfo);

        m_RHIHandle.Sync(RHI::ResourceKind::Buffer, VkHandleToU64(m_Buffer), RHI::Backend::Vulkan);
        // Root-object registration so the dispatch path can resolve a
        // BindStorageBuffer packet's handle back to this object (#691
        // Phase 7). Identity is preserved across Resize (Sync), so
        // re-registering the same key just refreshes the same entry.
        VulkanRootObjectRegistry::Get().Register(m_RHIHandle.Get(), VulkanRootObjectKind::StorageBuffer, this);
        // GL twins occupy their binding point from creation (glBindBufferBase
        // in the ctor); mirror that so a pass that never calls Bind() still
        // resolves.
        VulkanBindingState::Get().SetStorageBuffer(m_Binding, this);
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
        // No driver bind — the address travels in root data (ADR 0011 §4).
        // What Bind() DOES mean here is GL's glBindBufferBase semantics:
        // publish this buffer as binding point m_Binding's occupant so the
        // draw-time root writer can find it.
        VulkanBindingState::Get().SetStorageBuffer(m_Binding, const_cast<VulkanStorageBuffer*>(this));
    }

    void VulkanStorageBuffer::Unbind() const
    {
        auto& state = VulkanBindingState::Get();
        if (state.GetStorageBuffer(m_Binding) == this)
        {
            state.SetStorageBuffer(m_Binding, nullptr);
        }
    }

    void VulkanStorageBuffer::SetData(const void* data, u32 size, u32 offset)
    {
        OLO_PROFILE_FUNCTION();

        if (data == nullptr || size == 0)
        {
            return;
        }
        if (offset + size > std::max(m_Size, 1u))
        {
            OLO_CORE_ERROR("VulkanStorageBuffer::SetData: {}+{} exceeds the buffer's {} bytes — dropping", offset,
                           size, m_Size);
            return;
        }

        // NOTE (in-flight caveat, same as VulkanVertexBuffer): a direct
        // mapped write races a PREVIOUS frame's submitted reads of the same
        // range. Current DynamicDraw call sites write at load/setup time or
        // rewrite whole per-frame ranges whose consumers are recorded after
        // the write; a streaming ring lands with the wave that needs it.
        if (m_Mapped != nullptr)
        {
            std::memcpy(static_cast<u8*>(m_Mapped) + offset, data, size);
            if (m_NeedsFlush)
            {
                vmaFlushAllocation(VulkanDevice::Get()->GetAllocator(), m_Allocation, offset, size);
            }
            return;
        }

        VulkanOneShot::UploadToBuffer(m_Buffer, offset, data, size, "VulkanStorageBuffer::SetData");
    }

    void VulkanStorageBuffer::GetData(void* outData, u32 size, u32 offset) const
    {
        OLO_PROFILE_FUNCTION();

        if (outData == nullptr || size == 0)
        {
            return;
        }
        if (offset + size > std::max(m_Size, 1u))
        {
            OLO_CORE_ERROR("VulkanStorageBuffer::GetData: {}+{} exceeds the buffer's {} bytes", offset, size, m_Size);
            std::memset(outData, 0, size);
            return;
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Buffer == VK_NULL_HANDLE)
        {
            std::memset(outData, 0, size);
            return;
        }

        // Mid-frame (#691 Phase 8): the producing dispatch may still sit
        // unsubmitted in the frame command buffer, and queue submissions
        // execute in submit order — the one-shot below would read the
        // PREVIOUS frame's contents (plus a full GPU stall for nothing).
        // Submit-and-continue the frame first; a refused flush (headless
        // recording, backbuffer already written, open query) falls back to
        // the old behaviour with a warn-once, never silently.
        // Live-object probe, not the static flag — see SubImage's note.
        if (auto* vk = dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI());
            vk != nullptr && vk->CurrentCommandBuffer() != VK_NULL_HANDLE)
        {
            {
                auto* context = VulkanContext::Get();
                if (context == nullptr || !context->FlushFrameRecordingAndWait())
                {
                    static bool s_Warned = false;
                    if (!s_Warned)
                    {
                        s_Warned = true;
                        OLO_CORE_WARN("[Vulkan] mid-frame StorageBuffer::GetData without a frame flush — the "
                                      "readback may return the previous frame's contents");
                    }
                }
            }
        }

        // Always a one-shot copy, even on a mapped placement: a DynamicCopy
        // buffer's contents come from GPU writes, and the copy's barrier is
        // what makes those available to the host read.
        VkBufferCreateInfo readbackInfo{};
        readbackInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        readbackInfo.size = size;
        readbackInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readbackInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo readbackAlloc{};
        readbackAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        readbackAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo readbackOut{};
        if (vmaCreateBuffer(device->GetAllocator(), &readbackInfo, &readbackAlloc, &readback, &readbackAllocation,
                            &readbackOut) != VK_SUCCESS)
        {
            std::memset(outData, 0, size);
            return;
        }

        const bool ok = VulkanOneShot::Submit(
            "VulkanStorageBuffer::GetData",
            [&](VkCommandBuffer cmd)
            {
                // Make any prior GPU writes to the source range available to
                // the copy first.
                VkBufferMemoryBarrier2 pre{};
                pre.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                pre.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                pre.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                pre.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                pre.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                pre.buffer = m_Buffer;
                pre.offset = offset;
                pre.size = size;
                VkDependencyInfo dep{};
                dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dep.bufferMemoryBarrierCount = 1u;
                dep.pBufferMemoryBarriers = &pre;
                vkCmdPipelineBarrier2(cmd, &dep);

                VkBufferCopy region{};
                region.srcOffset = offset;
                region.dstOffset = 0;
                region.size = size;
                vkCmdCopyBuffer(cmd, m_Buffer, readback, 1u, &region);
            });

        if (ok)
        {
            vmaInvalidateAllocation(device->GetAllocator(), readbackAllocation, 0, size);
            std::memcpy(outData, readbackOut.pMappedData, size);
        }
        else
        {
            std::memset(outData, 0, size);
        }
        vmaDestroyBuffer(device->GetAllocator(), readback, readbackAllocation);
    }

    void VulkanStorageBuffer::ClearData()
    {
        OLO_PROFILE_FUNCTION();

        auto* device = VulkanDevice::Get();
        if (device == nullptr || m_Buffer == VK_NULL_HANDLE || m_Size == 0)
        {
            return;
        }

        // Mid-frame (#691 Phase 8): a ClearData between two GPU uses
        // (ToneMap's exposure-reset shape, the fluid solver's grid-head
        // clears) must be ORDERED within the frame command buffer — both the
        // one-shot below (submits BEFORE the still-recording frame) and the
        // mapped memset (a CPU write the frame's earlier-recorded dispatches
        // would observe at submit time) break that ordering. Route through
        // the facade's frame-CB fill, which ends the rendering scope and
        // brackets the fill with the right barriers. This check deliberately
        // PRECEDES the mapped fast path.
        // Live-object probe, not the static flag — see SubImage's note.
        if (auto* vk = dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI());
            vk != nullptr && vk->CurrentCommandBuffer() != VK_NULL_HANDLE)
        {
            vk->ClearBufferUInt(m_RHIHandle.Get(), 0u);
            return;
        }

        if (m_Mapped != nullptr)
        {
            std::memset(m_Mapped, 0, m_Size);
            if (m_NeedsFlush)
            {
                vmaFlushAllocation(device->GetAllocator(), m_Allocation, 0, m_Size);
            }
            return;
        }

        // Load-time/one-shot fill (no frame recording live, so submit order
        // cannot invert anything).
        VulkanOneShot::Submit("VulkanStorageBuffer::ClearData",
                              [&](VkCommandBuffer cmd)
                              {
                                  vkCmdFillBuffer(cmd, m_Buffer, 0, VK_WHOLE_SIZE, 0u);

                                  VkBufferMemoryBarrier2 post{};
                                  post.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                                  post.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
                                  post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                                  post.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                                  post.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                                  post.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                  post.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                  post.buffer = m_Buffer;
                                  post.offset = 0;
                                  post.size = VK_WHOLE_SIZE;
                                  VkDependencyInfo dep{};
                                  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                                  dep.bufferMemoryBarrierCount = 1u;
                                  dep.pBufferMemoryBarriers = &post;
                                  vkCmdPipelineBarrier2(cmd, &dep);
                              });
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

    // =========================================================================
    // VulkanRawTextureRegistry (#691 Phase 8 — see the header comment)
    // =========================================================================

    VulkanRawTextureRegistry& VulkanRawTextureRegistry::Get()
    {
        static auto* s_Instance = new VulkanRawTextureRegistry(); // deliberately leaked
        return *s_Instance;
    }

    RHI::ResourceHandle VulkanRawTextureRegistry::Adopt(Ref<VulkanTexture2D> texture)
    {
        if (texture == nullptr)
        {
            return {};
        }
        const RHI::ResourceHandle handle = texture->GetRHIHandle();
        if (!handle.IsValid())
        {
            return {};
        }
        m_Entries[Key(handle)] = Entry{ .Texture2D = std::move(texture) };
        return handle;
    }

    RHI::ResourceHandle VulkanRawTextureRegistry::Adopt(Ref<VulkanTextureCubemap> cubemap)
    {
        if (cubemap == nullptr)
        {
            return {};
        }
        const RHI::ResourceHandle handle = cubemap->GetRHIHandle();
        if (!handle.IsValid())
        {
            return {};
        }
        m_Entries[Key(handle)] = Entry{ .Cubemap = std::move(cubemap) };
        return handle;
    }

    Ref<VulkanTexture2D> VulkanRawTextureRegistry::Lookup2D(RHI::ResourceHandle handle) const
    {
        const auto it = m_Entries.find(Key(handle));
        return it != m_Entries.end() ? it->second.Texture2D : nullptr;
    }

    bool VulkanRawTextureRegistry::Contains(RHI::ResourceHandle handle) const
    {
        return m_Entries.contains(Key(handle));
    }

    bool VulkanRawTextureRegistry::Destroy(RHI::ResourceHandle handle)
    {
        // Erasing drops the owning Ref: the texture destructor retires the
        // identity (outstanding handles go stale) and routes the VkImage
        // through VulkanDeferredReclaim — never an inline destroy.
        return m_Entries.erase(Key(handle)) != 0u;
    }

    void VulkanRawTextureRegistry::ReleaseAll()
    {
        m_Entries.clear();
    }

    // =========================================================================
    // VulkanRawFramebufferRegistry (#691 Phase 8 — see the header comment)
    // =========================================================================

    VulkanRawFramebufferRegistry& VulkanRawFramebufferRegistry::Get()
    {
        static auto* s_Instance = new VulkanRawFramebufferRegistry(); // deliberately leaked
        return *s_Instance;
    }

    RHI::ResourceHandle VulkanRawFramebufferRegistry::Adopt(Ref<VulkanFramebuffer> framebuffer)
    {
        if (framebuffer == nullptr)
        {
            return {};
        }
        const RHI::ResourceHandle handle = framebuffer->GetRHIHandle();
        if (!handle.IsValid())
        {
            return {};
        }
        m_Entries[Key(handle)] = std::move(framebuffer);
        return handle;
    }

    bool VulkanRawFramebufferRegistry::Contains(RHI::ResourceHandle handle) const
    {
        return m_Entries.contains(Key(handle));
    }

    bool VulkanRawFramebufferRegistry::Destroy(RHI::ResourceHandle handle)
    {
        // The framebuffer destructor unregisters from VulkanRootObjectRegistry
        // and retires the identity; the attachment Refs it holds release with
        // it (each routing through VulkanDeferredReclaim if this was the last
        // owner).
        return m_Entries.erase(Key(handle)) != 0u;
    }

    void VulkanRawFramebufferRegistry::ReleaseAll()
    {
        m_Entries.clear();
    }

#undef OLO_VK_PHASE6_STUB
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
