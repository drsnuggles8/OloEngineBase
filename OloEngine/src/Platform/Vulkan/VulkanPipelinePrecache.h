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
//   whatever exceeds it waits for the next frame. Nothing ever blocks on the
//   queue: a draw that arrives first creates its own pipeline and is counted as
//   requested too late.
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
#include <unordered_map>

namespace OloEngine
{
    class VulkanComputeShader;
    class VulkanShader;

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
        // The renderer publishes its tag every frame graph compile. The first
        // tag a session sees is selected at once; a change is selected only
        // after it has been the one tag published for kTagSettleTicks frames,
        // so two renderers drawing different configurations in one frame (a
        // viewport and a preview) cannot make the selection flip every frame.
        // Draw creations are always recorded under the tag published last.
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
        [[nodiscard]] PipelineManifest CopyManifest() const;
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
        void ApplySelectedTagLocked(std::string_view tag);
        void SaveManifestLocked();
        void WorkerLoop();
        void StopWorkers();

        mutable std::mutex m_Mutex;
        bool m_Started = false;
        bool m_Persist = false;
        PipelineManifest m_Manifest;
        FString m_Tag;          ///< The selected configuration: what requests are prioritised and judged by.
        FString m_PublishedTag; ///< The tag published last: what draw creations are recorded under.
        FString m_FrameTag;     ///< The tag published this frame, when only one was.
        bool m_FrameTagSeen = false;
        bool m_FrameTagMixed = false; ///< More than one distinct tag was published this frame.
        FString m_CandidateTag;
        u32 m_CandidateTicks = 0;
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
        bool m_StopRequested = false;
        static constexpr u32 kMaxWorkers = 8;
        std::array<std::thread, kMaxWorkers> m_Workers;
        u32 m_WorkerCount = 0;
    };
} // namespace OloEngine

#endif // OLO_WITH_VULKAN
