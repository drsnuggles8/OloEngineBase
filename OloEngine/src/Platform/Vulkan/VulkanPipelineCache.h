#pragma once

// VulkanPipelineCache — the process-wide VkPipelineCache plus the
// shader→pipeline reverse index. Issue #691 Phase 6, ADR 0011 §3(c)/(d).
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
// REVERSE INDEX (§3(d)). On Vulkan one shader is the source of N pipelines
// (state permutation × attachment formats), so Shader::Reload() cannot stay a
// per-object operation: RegisterPipeline records shader→pipeline at creation
// time, and InvalidateShader destroys every dependent pipeline — DEFERRED
// through VulkanDeferredReclaim (an in-flight command buffer may still
// reference them) and LAZILY recreated on next use (eager recreation of every
// permutation per save would stall the iteration loop hot reload exists to
// serve; the recreated pipeline hits this cache for everything that did not
// change).
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

        // Where the blob lives: <working-dir>/assets/cache/shader/vulkan/
        // (created on demand). Separate from the GL tiers' directory so the
        // two backends' cache stories stay independently deletable.
        [[nodiscard]] static std::filesystem::path CacheFilePath();

      private:
        VulkanPipelineCache() = default;

        VkPipelineCache m_Cache = VK_NULL_HANDLE;
        bool m_LoadAttempted = false;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
