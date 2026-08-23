#pragma once

// VulkanPipelineCache — the process-wide VkPipelineCache plus the
// shader→pipeline reverse index. Issue #691, ADR 0011 §3(c)/(d).
//
// DISK BLOB (§3(c)). One VkPipelineCache for the whole process, serialised to
// <cache>/pipeline_cache.vkpc. NO side-car driver stamp: the blob's own
// VkPipelineCacheHeaderVersionOne carries vendor/device/pipelineCacheUUID and
// the driver is REQUIRED to safely ignore a blob it does not recognise —
// hand-rolling a stamp would reimplement a guarantee the API already gives
// (unlike GL's program binaries, where the stamp exists because rejection
// behaviour is vendor-dependent and crash-prone).
//
// SOFT-FAIL IS THE CONTRACT. A missing, truncated, unreadable or rejected
// blob costs a slower first frame and a log line, never a failed launch; a
// failed save at shutdown is logged and ignored. This mirrors the GL path's
// rejected-glProgramBinary fallback.
//
// §3(d)'s shader→pipeline reverse index does NOT live here — it lives in
// VulkanPipelineBuilder, fused with the key→pipeline lookup map (see the note
// on the class below). This class is the disk blob only; the lazily-recreated
// pipelines after an invalidation hit it for everything that did not change.
//
// Thread-safety: NONE, deliberately — render thread only.

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include <volk.h>

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class VulkanPipelineCache
    {
      public:
        // Process-wide instance, deliberately leaked (see
        // VulkanImageInfoRegistry::Get for the rationale).
        [[nodiscard]] static VulkanPipelineCache& Get();

        // The native cache for vkCreate*Pipelines calls. Lazily created on
        // first use (loads the disk blob then; requires a live VulkanDevice).
        // VK_NULL_HANDLE when no device is up — passing that to a pipeline
        // create is legal (it just means "no cache").
        [[nodiscard]] VkPipelineCache Handle();

        // Serialise to disk (log-and-continue on any failure), then destroy
        // the native cache. Shutdown / device-teardown path — the device must
        // still be alive.
        void SaveAndDestroy();

        // NOTE: §3(d)'s shader→pipeline reverse index lives in
        // VulkanPipelineBuilder (whose key→pipeline map must be invalidated in
        // the same act — a second owner here caused a double-destroy on the
        // first device run). This class is the DISK BLOB only.

        // Where the blob lives: ShaderCachePaths::Root()/vulkan/pipeline_cache.vkpc
        // (created on demand; relocated out of the source tree behind
        // OLO_SHADER_CACHE_DIR by issue #906). Separate subdirectory from the
        // GL tiers so the two backends' cache stories stay independently
        // deletable.
        [[nodiscard]] static std::filesystem::path CacheFilePath();

      private:
        VulkanPipelineCache() = default;

        VkPipelineCache m_Cache = VK_NULL_HANDLE;
        bool m_LoadAttempted = false;
        bool m_CreateFailed = false; ///< Hard vkCreatePipelineCache failure — do not retry per Handle() call.
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
