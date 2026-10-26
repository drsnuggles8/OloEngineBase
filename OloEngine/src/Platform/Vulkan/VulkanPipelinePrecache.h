#pragma once

// VulkanPipelinePrecache: collects complete PSO requests from content and the
// selected rendering configuration, and prepares them off the draw threads
// within a per-frame budget (issue #1345). VulkanPipelineBuilder owns the
// pipelines, the request ledger and the creation itself; this class decides
// WHAT to request and WHEN, and says whether a shader is render-ready.
//
// WHERE REQUESTS COME FROM
//   Content: every VulkanShader / VulkanComputeShader reports itself here when
//     its modules exist (construction, hot reload) and when it dies. A shader
//     comes into existence when content needs it — a library load, a pass's
//     Init(), a material asset naming a custom shader, a shader graph, a
//     streamed region's assets — so this is the content hook, and it needs
//     nothing from the streaming system (#434) to work.
//   Configuration: the renderer publishes a configuration tag (path, G-Buffer
//     sample count, upscale mode, OIT; MakePipelineConfigurationTag) whenever
//     its frame graph declaration changes.
//   The complete key: the PipelineManifest, recorded from the draws that had to
//     create pipelines, says which (formats, samples, patch size) each shader
//     identity has needed under which configuration. A live shader is requested
//     against every entry: the selected configuration's entries first, other
//     configurations' after them (speculatively, so an MSAA or path switch the
//     project has made before finds its pipelines ready).
//
// WHEN WORK RUNS
//   Requests are made on the thread that reports the shader (it snapshots the
//   modules). Creation runs on dedicated worker threads, never inside a
//   recording region. Tick() runs once per frame on the render thread, outside
//   recording, and hands the workers that frame's budget (JobsPerFrame starts);
//   whatever exceeds it waits for the next frame. While no frame comes (loading)
//   the workers refill the same budget every 33 ms themselves. Nothing ever
//   blocks on the queue: a draw that arrives first creates its own pipeline and
//   is counted as requested too late.
//
// READINESS
//   A tracked shader is RENDER-READY when every request it has for the selected
//   configuration is created. The claim goes to the builder's ledger, so a draw
//   that then needs a pipeline nobody requested for that shader is counted as a
//   ReadyClaimViolation and logged, instead of hiding behind the claim. A shader
//   the manifest does not know yet is reported UNKNOWN, never ready: its first
//   draw is a counted never-requested miss, recorded so the next session
//   requests it.
//
// Locking: m_Mutex guards everything below; it is taken BEFORE the builder's
// lock (requests), never while holding it (the builder records draw creations
// after releasing its own).

#include "OloEngine/Core/Base.h"

#if OLO_WITH_VULKAN

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Renderer/PipelineManifest.h"
#include "OloEngine/Renderer/PipelineReadiness.h"
#include "Platform/Vulkan/VulkanPipelineBuilder.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <array>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class VulkanComputeShader;
    class VulkanShader;

    // A key a draw created or refreshed, waiting to be merged into the manifest.
    struct VulkanPipelinePrecacheRecord
    {
        FString Shader;
        u32 StageMask = 0;
        PipelineSignature Signature;
        FString Tag;
    };

    template<>
    struct TIsTriviallyRelocatable<VulkanPipelinePrecacheRecord>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(VulkanPipelinePrecacheRecord::Shader)> &&
                                      TIsTriviallyRelocatable_V<decltype(VulkanPipelinePrecacheRecord::StageMask)> &&
                                      TIsTriviallyRelocatable_V<decltype(VulkanPipelinePrecacheRecord::Signature)> &&
                                      TIsTriviallyRelocatable_V<decltype(VulkanPipelinePrecacheRecord::Tag)>;
    };

    class VulkanPipelinePrecache
    {
      public:
        [[nodiscard]] static VulkanPipelinePrecache& Get();

        struct StartOptions
        {
            bool LoadAndSaveManifest = true; ///< Read the shipped seed + machine manifest, save at Stop().
            bool StartWorkers = true;        ///< False: requests are prepared only by PrepareQueued (tests).
        };

        // Device bring-up / teardown (VulkanContext). Stop() joins the workers
        // and saves the manifest; it must run before the builder's ReleaseAll.
        void Start(const StartOptions& options);
        void Stop();
        [[nodiscard]] bool IsStarted() const;

        // --- Content -----------------------------------------------------------
        void OnShaderReady(VulkanShader& shader);
        void OnShaderReloaded(VulkanShader& shader, const TArray<VulkanPipelineBuilder::Key>& invalidated);
        void OnShaderDestroyed(const VulkanShader& shader);
        void OnComputeShaderReady(VulkanComputeShader& shader);
        void OnComputeShaderReloaded(VulkanComputeShader& shader, const TArray<VulkanPipelineBuilder::Key>& invalidated);
        void OnComputeShaderDestroyed(const VulkanComputeShader& shader);

        // --- Configuration -----------------------------------------------------
        // The renderer publishes its tag every frame graph compile. The
        // SELECTED configuration is the set of tags drawn in a frame: one tag
        // normally, two when a viewport and a preview with different settings
        // render in the same frame. The first tag a session sees is selected at
        // once; a change is selected after the same set has been drawn for
        // kTagSettleTicks frames. Draw creations are recorded under the tag
        // published last.
        void SetConfigurationTag(std::string_view tag);
        [[nodiscard]] std::string GetConfigurationTag() const;
        static constexpr u32 kTagSettleTicks = 2;

        // The builder created `signature` for `shader` on a draw thread.
        void RecordDrawCreation(std::string_view shader, u32 stageMask, const PipelineSignature& signature);

        // Once per frame, render thread, outside every recording region.
        void Tick();

        // Prepare up to `maxJobs` queued requests on the calling thread; what the
        // workers do, made deterministic for tests. Returns how many ran.
        sizet PrepareQueued(sizet maxJobs);

        // Fill the Vulkan half of a readiness snapshot (re-evaluates readiness).
        void FillSnapshot(PipelineReadinessSnapshot& snapshot);
        void ResetCounters();

        // Write the manifest now if this session persists it and it changed
        // (Stop() does the same). Build Game calls it before staging the file,
        // so a packaged runtime gets this session's records too.
        void SaveManifest();

        // The manifest, for tests and for the game build's staging step.
        [[nodiscard]] PipelineManifest CopyManifest();
        void SetManifestForTesting(PipelineManifest manifest);

        static constexpr u32 kDefaultJobsPerFrame = 4;
        static constexpr u32 kDefaultWorkers = 2;
        static constexpr sizet kMaxPendingShadersReported = 32;

      private:
        VulkanPipelinePrecache() = default;

        struct Tracked
        {
            FString Name;
            u32 StageMask = 0;
            u64 ShaderKey = 0;
            bool Compute = false;
            bool HasSelectedEntries = false;
            bool DeclaredReady = false;
            TArray<VulkanPipelineBuilder::Key> SelectedKeys;
        };

        // Lock held. Requests every manifest entry for one tracked shader.
        void RequestFromManifest(Tracked& tracked, VulkanShader* graphics, VulkanComputeShader* compute);
        void RequestAllLocked();
        void EvaluateReadinessLocked(bool force);
        void ApplySelectedTagsLocked(const TArray<FString>& tags);
        void RevokeReadyClaimsLocked();
        [[nodiscard]] bool IsSelectedLocked(std::string_view tag) const;
        [[nodiscard]] bool ClaimsAllowedLocked() const;
        void DrainPendingRecordsLocked();
        void SaveManifestLocked();
        void WorkerLoop();
        void StopWorkers();

        mutable std::mutex m_Mutex;
        bool m_Started = false;
        bool m_Persist = false;
        PipelineManifest m_Manifest;
        TArray<FString> m_SelectedTags;  ///< Sorted: what requests are prioritised and judged by.
        FString m_PublishedTag;          ///< The tag published last.
        TArray<FString> m_FrameTags;     ///< Tags published since the last Tick.
        TArray<FString> m_LastFrameTags; ///< The previous frame's tags (sorted).
        TArray<FString> m_CandidateTags; ///< A different set waiting to settle.
        u32 m_CandidateTicks = 0;

        // Draw records arrive from recording threads; they take only this
        // small lock and are merged into the manifest under m_Mutex at Tick,
        // so a hit's manifest refresh never waits for readiness or a save.
        std::mutex m_RecordMutex;
        FString m_RecordTag; ///< m_PublishedTag, as the recording threads see it.
        TArray<VulkanPipelinePrecacheRecord> m_PendingRecords;
        std::unordered_map<const VulkanShader*, Tracked> m_Graphics;
        std::unordered_map<const VulkanComputeShader*, Tracked> m_Compute;
        u64 m_EvaluatedCompletions = ~0ull;
        bool m_ReadinessDirty = true;
        PipelineReadinessSnapshot m_LastReadiness;
        u32 m_JobsPerFrame = kDefaultJobsPerFrame;

        // Workers: Tick() grants m_Tokens; each started creation spends one.
        std::mutex m_WorkMutex;
        std::condition_variable m_WorkCv;
        u32 m_Tokens = 0;
        std::chrono::steady_clock::time_point m_LastFrameTick{}; ///< The last Tick (a real frame).
        std::chrono::steady_clock::time_point m_LastGrant{};     ///< The last budget grant, Tick or refill.
        // With no frame for kNoFrameThreshold the process is loading, and the
        // workers grant themselves the per-frame budget every
        // kLoadingRefillInterval (see WorkerLoop). A slow frame (under 4 fps)
        // is not loading; the per-frame bound holds for it.
        static constexpr std::chrono::milliseconds kNoFrameThreshold{ 250 };
        static constexpr std::chrono::milliseconds kLoadingRefillInterval{ 33 };
        bool m_StopRequested = false;
        static constexpr u32 kMaxWorkers = 8;
        std::array<std::thread, kMaxWorkers> m_Workers;
        u32 m_WorkerCount = 0;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
