#include "OloEnginePCH.h"

#if OLO_WITH_VULKAN

#include "Platform/Vulkan/VulkanPipelineCache.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTransientResources.h"
#include "OloEngine/Renderer/ShaderCachePaths.h"

#include <fstream>

namespace OloEngine
{
    VulkanPipelineCache& VulkanPipelineCache::Get()
    {
        static auto* s_Instance = new VulkanPipelineCache(); // deliberately leaked
        return *s_Instance;
    }

    std::filesystem::path VulkanPipelineCache::CacheFilePath()
    {
        // Relocated behind OLO_SHADER_CACHE_DIR (issue #906), same root as the
        // portable SPIR-V tiers — safe to co-locate because this blob is
        // driver-produced and the driver itself is the guard: an unrecognised
        // VkPipelineCacheHeaderVersionOne (wrong vendor/device/UUID) is
        // required to be rejected at vkCreatePipelineCache, handled below by
        // the soft-fail/recreate-empty path.
        return ShaderCachePaths::Root() / "vulkan" / "pipeline_cache.vkpc";
    }

    VkPipelineCache VulkanPipelineCache::Handle()
    {
        if (m_Cache != VK_NULL_HANDLE || m_CreateFailed)
        {
            // A hard create failure is recorded once — Handle() runs per
            // pipeline creation, and retrying a persistently failing driver
            // call would emit one failed call + one warn per pipeline.
            return m_Cache;
        }

        auto* device = VulkanDevice::Get();
        if (device == nullptr)
        {
            return VK_NULL_HANDLE;
        }

        // One load attempt per device lifetime — a rejected blob must not be
        // re-read every Handle() call.
        std::vector<char> blob;
        if (!m_LoadAttempted)
        {
            m_LoadAttempted = true;
            std::error_code ec;
            const auto path = CacheFilePath();
            if (std::filesystem::exists(path, ec) && !ec)
            {
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (file)
                {
                    const std::streamsize size = file.tellg();
                    if (size > 0)
                    {
                        blob.resize(static_cast<sizet>(size));
                        file.seekg(0);
                        if (!file.read(blob.data(), size))
                        {
                            // Truncated/unreadable → discard and start empty.
                            OLO_CORE_WARN("[Vulkan] pipeline_cache.vkpc unreadable — starting with an empty pipeline cache");
                            blob.clear();
                        }
                    }
                }
            }
        }

        VkPipelineCacheCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        createInfo.initialDataSize = blob.size();
        createInfo.pInitialData = blob.empty() ? nullptr : blob.data();

        VkResult result = vkCreatePipelineCache(device->GetDevice(), &createInfo, nullptr, &m_Cache);
        if (result != VK_SUCCESS && !blob.empty())
        {
            // §3(c) soft-fail: a rejected blob costs a slower first frame,
            // never a failed launch. (The driver is required to ignore an
            // unrecognised blob, but "required" and "does" have met before.)
            OLO_CORE_WARN("[Vulkan] vkCreatePipelineCache rejected the on-disk blob (VkResult {}) — recreating empty",
                          static_cast<int>(result));
            createInfo.initialDataSize = 0;
            createInfo.pInitialData = nullptr;
            result = vkCreatePipelineCache(device->GetDevice(), &createInfo, nullptr, &m_Cache);
        }
        if (result != VK_SUCCESS)
        {
            OLO_CORE_WARN("[Vulkan] vkCreatePipelineCache failed outright (VkResult {}) — pipelines will be uncached",
                          static_cast<int>(result));
            m_Cache = VK_NULL_HANDLE;
            m_CreateFailed = true;
        }
        else if (!blob.empty())
        {
            OLO_CORE_INFO("[Vulkan] Pipeline cache loaded ({} bytes)", blob.size());
        }
        return m_Cache;
    }

    void VulkanPipelineCache::SaveAndDestroy()
    {
        auto* device = VulkanDevice::Get();
        if (m_Cache == VK_NULL_HANDLE || device == nullptr)
        {
            m_Cache = VK_NULL_HANDLE;
            m_LoadAttempted = false;
            m_CreateFailed = false;
            return;
        }

        // Save: every failure is logged and ignored (§3(c) — a lost cache is a
        // cold start, not an error).
        do
        {
            sizet dataSize = 0;
            if (vkGetPipelineCacheData(device->GetDevice(), m_Cache, &dataSize, nullptr) != VK_SUCCESS || dataSize == 0)
            {
                break;
            }
            std::vector<char> blob(dataSize);
            if (vkGetPipelineCacheData(device->GetDevice(), m_Cache, &dataSize, blob.data()) != VK_SUCCESS)
            {
                break;
            }

            const auto path = CacheFilePath();
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec)
            {
                OLO_CORE_WARN("[Vulkan] Could not create pipeline-cache directory {} — cache not saved",
                              path.parent_path().string());
                break;
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file || !file.write(blob.data(), static_cast<std::streamsize>(dataSize)))
            {
                OLO_CORE_WARN("[Vulkan] Failed writing {} — pipeline cache not saved", path.string());
                break;
            }
            OLO_CORE_INFO("[Vulkan] Pipeline cache saved ({} bytes)", dataSize);
        } while (false);

        vkDestroyPipelineCache(device->GetDevice(), m_Cache, nullptr);
        m_Cache = VK_NULL_HANDLE;
        m_LoadAttempted = false;
        m_CreateFailed = false;
    }
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
