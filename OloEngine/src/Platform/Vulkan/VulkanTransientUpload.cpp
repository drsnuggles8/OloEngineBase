#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanTransientUpload.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

#include <stdexcept>
#include <string>
#ifdef OLO_DEBUG
#include <mutex>
#include <stacktrace>
#include <unordered_map>
#include <utility>
#endif

namespace OloEngine::VulkanUpload
{
#ifdef OLO_DEBUG
    namespace
    {
        // Teardown forensics for object textures / storage buffers (#691
        // The close-button VMA abort): a Ref surviving the full
        // renderer teardown keeps its VMA allocation alive into
        // vmaDestroyAllocator. The VAO twin lives in VulkanBufferResources
        // (LogSurvivingVertexArrays); this covers the classes owned by the
        // transient-resource TUs.
        std::mutex s_LiveTrackMutex;
        std::unordered_map<const void*, std::pair<const char*, std::stacktrace>> s_LiveGpuObjects;
    } // namespace

    void TrackLive(const void* object, const char* what)
    {
        std::scoped_lock lock(s_LiveTrackMutex);
        s_LiveGpuObjects.emplace(object, std::make_pair(what, std::stacktrace::current(2)));
    }
    void UntrackLive(const void* object)
    {
        std::scoped_lock lock(s_LiveTrackMutex);
        s_LiveGpuObjects.erase(object);
    }
#else
    void TrackLive(const void*, const char*)
    {
    }
    void UntrackLive(const void*)
    {
    }
#endif

    void VkCheck(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error(std::string("Vulkan bring-up: ") + what + " failed (VkResult " +
                                     std::to_string(static_cast<int>(result)) + ")");
        }
    }

    VkFormat ImageFormatToVkFormat(ImageFormat format, bool srgb)
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
            case ImageFormat::R32UI:
                return VK_FORMAT_R32_UINT;
            case ImageFormat::RG8:
                return VK_FORMAT_R8G8_UNORM;
            case ImageFormat::BC7:
                return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
            case ImageFormat::BC5:
                return VK_FORMAT_BC5_UNORM_BLOCK;
            case ImageFormat::BC4:
                // NOTE: BC4 samples (R, 0, 0, 1) and this engine's contract is
                // (R, R, R, 1) — the OpenGL upload installs a texture swizzle for it
                // (OpenGLTexture2D's compressed constructor). Vulkan's equivalent is a
                // VkComponentMapping on the image view, and there is none in the tree
                // yet. Latent today because the Vulkan compressed-texture upload path
                // is still a stub (#691); whoever enables it must add the mapping, or
                // every BC4 texture reads green and blue as zero.
                return VK_FORMAT_BC4_UNORM_BLOCK;
            case ImageFormat::BC6H:
                return VK_FORMAT_BC6H_UFLOAT_BLOCK;
            case ImageFormat::BC6HS:
                return VK_FORMAT_BC6H_SFLOAT_BLOCK;
            case ImageFormat::RGBA32UI:
                return VK_FORMAT_R32G32B32A32_UINT;
        }

        OLO_CORE_ASSERT(false, "ImageFormatToVkFormat: unknown ImageFormat {}", static_cast<u32>(format));
        return VK_FORMAT_UNDEFINED;
    }

    u32 EngineFormatClientBpp(ImageFormat format)
    {
        switch (format)
        {
            case ImageFormat::None:
            case ImageFormat::DEPTH24STENCIL8:
            case ImageFormat::BC7:
            case ImageFormat::BC5:
            case ImageFormat::BC6H:
            case ImageFormat::BC6HS:
            case ImageFormat::BC4:
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
            case ImageFormat::R32UI:
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

    std::vector<u8> ExpandRgbToRgba(ImageFormat format, const void* data, u64 pixelCount)
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

    void RecordImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, u32 baseMip, u32 mipCount,
                            u32 baseLayer, u32 layerCount)
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

    VulkanRendererAPI* TryGetRecordingVulkanAPI()
    {
        auto* vk = dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI());
        if (vk == nullptr || vk->CurrentCommandBuffer() == VK_NULL_HANDLE)
        {
            return nullptr;
        }
        return vk;
    }

    VulkanRendererAPI* TryGetVulkanAPI()
    {
        return dynamic_cast<VulkanRendererAPI*>(&RenderCommand::GetRendererAPI());
    }

} // namespace OloEngine::VulkanUpload

namespace OloEngine
{
    // At OloEngine scope deliberately: a cross-module teardown entry
    // point (VulkanContext.cpp calls it unqualified via the
    // VulkanTransientResources.h umbrella). It reads VulkanUpload's
    // tracking state, hence living in this TU.
    void VulkanLogSurvivingTransients()
    {
#ifdef OLO_DEBUG
        std::scoped_lock lock(VulkanUpload::s_LiveTrackMutex);
        for (const auto& [object, info] : VulkanUpload::s_LiveGpuObjects)
        {
            std::string trace = std::to_string(info.second);
            sizet cut = 0;
            for (int newlines = 0; cut < trace.size(); ++cut)
            {
                if (trace[cut] == '\n' && ++newlines == 12)
                    break;
            }
            OLO_CORE_ERROR("[Vulkan] surviving {} created at:\n{}", info.first, trace.substr(0, cut));
        }
        if (!VulkanUpload::s_LiveGpuObjects.empty())
        {
            OLO_CORE_ERROR("[Vulkan] {} texture/storage-buffer object(s) survived full teardown", VulkanUpload::s_LiveGpuObjects.size());
        }
#endif
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
