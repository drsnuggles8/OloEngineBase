#pragma once

// What "ready to draw" means for a pipeline on each backend, and the counters
// that measure it (issue #1345).
//
// THE TWO BACKENDS DO NOT HAVE THE SAME OBJECT, and this header does not
// pretend they do:
//
//   Vulkan  A draw needs a complete VkPipeline: shader stages + root-data layout
//           + attachment formats + sample count + patch size (the key in
//           VulkanPipelineBuilder). Creating one is the driver's compile. A
//           pipeline is READY when vkCreateGraphicsPipelines has returned it, and
//           everything the precache measures (prepared first uses, misses, the
//           time a draw blocked on creation, driver-cache hits) is a fact about
//           that object.
//
//   OpenGL  There is no pipeline object. A program is READY when its link has
//           completed (GL_COMPLETION_STATUS under parallel shader compile, or the
//           synchronous link). What can be measured is the compile, the link,
//           a program-binary cache hit, and the case where a draw had to force a
//           link that had not finished (the GL analogue of a late request). What
//           cannot be measured is the driver's own lazy recompile on the first
//           draw under new state: GL exposes no signal for it, so it is reported
//           as unmeasurable, never as zero.
//
// The shader-preparation counters below are process-wide and backend-neutral in
// shape; each backend increments the ones that exist for it.

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PipelineRequestLedger.h"

#include <atomic>

namespace OloEngine
{
    enum class PipelineReadinessModel : u8
    {
        None,                  ///< No backend, or one that prepares nothing (null RHI).
        VulkanPipelineObjects, ///< Complete PSOs, requested ahead and created off the draw threads.
        OpenGLProgramLink,     ///< Program link only; the driver's first-draw work is unmeasurable.
    };

    [[nodiscard]] constexpr const char* ToString(const PipelineReadinessModel model)
    {
        switch (model)
        {
            case PipelineReadinessModel::None:
                return "none";
            case PipelineReadinessModel::VulkanPipelineObjects:
                return "vulkan-pipeline-objects";
            case PipelineReadinessModel::OpenGLProgramLink:
                return "opengl-program-link";
        }
        return "none";
    }

    // A plain copy of the counters for a snapshot.
    struct ShaderPreparationStats
    {
        u64 SourceCompiles = 0; ///< GLSL -> SPIR-V compiles (shaderc), or GL glCompileShader from text.
        f64 SourceCompileMs = 0.0;
        u64 SpirvCacheHits = 0; ///< Stages served from the on-disk SPIR-V cache instead of compiling.
        u64 ModuleCreates = 0;  ///< Vulkan vkCreateShaderModule calls.
        f64 ModuleCreateMs = 0.0;
        u64 ProgramLinks = 0; ///< GL glLinkProgram calls (issue time; an async link finishes later).
        f64 ProgramLinkIssueMs = 0.0;
        u64 ProgramBinaryHits = 0;    ///< GL programs restored from the program-binary cache.
        u64 ProgramBinaryRejects = 0; ///< GL program binaries the driver refused (a legal miss).
        u64 LinksForcedAtUse = 0;     ///< GL: a use (bind, raw program/handle fetch) forced an unfinished link.
        f64 LinksForcedAtUseMs = 0.0; ///< Time those uses blocked.
    };

    // Process-wide accumulators. Relaxed atomics: these are diagnostics, read
    // as a snapshot, never used for control flow.
    class ShaderPreparationCounters
    {
      public:
        [[nodiscard]] static ShaderPreparationCounters& Get()
        {
            static auto* s_Instance = new ShaderPreparationCounters(); // deliberately leaked, like ShaderRegistry
            return *s_Instance;
        }

        void AddSourceCompile(const f64 ms)
        {
            m_SourceCompiles.fetch_add(1, std::memory_order_relaxed);
            AddMicros(m_SourceCompileUs, ms);
        }
        void AddSpirvCacheHit()
        {
            m_SpirvCacheHits.fetch_add(1, std::memory_order_relaxed);
        }
        void AddModuleCreate(const f64 ms)
        {
            m_ModuleCreates.fetch_add(1, std::memory_order_relaxed);
            AddMicros(m_ModuleCreateUs, ms);
        }
        void AddProgramLink(const f64 issueMs)
        {
            m_ProgramLinks.fetch_add(1, std::memory_order_relaxed);
            AddMicros(m_ProgramLinkIssueUs, issueMs);
        }
        void AddProgramBinaryHit()
        {
            m_ProgramBinaryHits.fetch_add(1, std::memory_order_relaxed);
        }
        void AddProgramBinaryReject()
        {
            m_ProgramBinaryRejects.fetch_add(1, std::memory_order_relaxed);
        }
        void AddLinkForcedAtUse(const f64 ms)
        {
            m_LinksForcedAtUse.fetch_add(1, std::memory_order_relaxed);
            AddMicros(m_LinksForcedAtUseUs, ms);
        }

        [[nodiscard]] ShaderPreparationStats Snapshot() const
        {
            ShaderPreparationStats stats;
            stats.SourceCompiles = m_SourceCompiles.load(std::memory_order_relaxed);
            stats.SourceCompileMs = ToMs(m_SourceCompileUs);
            stats.SpirvCacheHits = m_SpirvCacheHits.load(std::memory_order_relaxed);
            stats.ModuleCreates = m_ModuleCreates.load(std::memory_order_relaxed);
            stats.ModuleCreateMs = ToMs(m_ModuleCreateUs);
            stats.ProgramLinks = m_ProgramLinks.load(std::memory_order_relaxed);
            stats.ProgramLinkIssueMs = ToMs(m_ProgramLinkIssueUs);
            stats.ProgramBinaryHits = m_ProgramBinaryHits.load(std::memory_order_relaxed);
            stats.ProgramBinaryRejects = m_ProgramBinaryRejects.load(std::memory_order_relaxed);
            stats.LinksForcedAtUse = m_LinksForcedAtUse.load(std::memory_order_relaxed);
            stats.LinksForcedAtUseMs = ToMs(m_LinksForcedAtUseUs);
            return stats;
        }

        void Reset()
        {
            for (auto* counter : { &m_SourceCompiles, &m_SourceCompileUs, &m_SpirvCacheHits, &m_ModuleCreates,
                                   &m_ModuleCreateUs, &m_ProgramLinks, &m_ProgramLinkIssueUs, &m_ProgramBinaryHits,
                                   &m_ProgramBinaryRejects, &m_LinksForcedAtUse, &m_LinksForcedAtUseUs })
            {
                counter->store(0, std::memory_order_relaxed);
            }
        }

      private:
        ShaderPreparationCounters() = default;

        static void AddMicros(std::atomic<u64>& target, const f64 ms)
        {
            target.fetch_add(static_cast<u64>(ms > 0.0 ? ms * 1000.0 : 0.0), std::memory_order_relaxed);
        }
        [[nodiscard]] static f64 ToMs(const std::atomic<u64>& micros)
        {
            return static_cast<f64>(micros.load(std::memory_order_relaxed)) / 1000.0;
        }

        std::atomic<u64> m_SourceCompiles{ 0 };
        std::atomic<u64> m_SourceCompileUs{ 0 };
        std::atomic<u64> m_SpirvCacheHits{ 0 };
        std::atomic<u64> m_ModuleCreates{ 0 };
        std::atomic<u64> m_ModuleCreateUs{ 0 };
        std::atomic<u64> m_ProgramLinks{ 0 };
        std::atomic<u64> m_ProgramLinkIssueUs{ 0 };
        std::atomic<u64> m_ProgramBinaryHits{ 0 };
        std::atomic<u64> m_ProgramBinaryRejects{ 0 };
        std::atomic<u64> m_LinksForcedAtUse{ 0 };
        std::atomic<u64> m_LinksForcedAtUseUs{ 0 };
    };

    // One shader whose selected-configuration requests are not all prepared.
    struct PipelineReadinessPendingShader
    {
        FString Shader;
        u32 Requested = 0; ///< Requests known for the selected configuration.
        u32 Prepared = 0;  ///< Of those, prepared.
        u32 Failed = 0;    ///< Of those, whose creation failed.
    };

    template<>
    struct TIsTriviallyRelocatable<PipelineReadinessPendingShader>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(PipelineReadinessPendingShader::Shader)> &&
                                      TIsTriviallyRelocatable_V<decltype(PipelineReadinessPendingShader::Requested)> &&
                                      TIsTriviallyRelocatable_V<decltype(PipelineReadinessPendingShader::Prepared)> &&
                                      TIsTriviallyRelocatable_V<decltype(PipelineReadinessPendingShader::Failed)>;
    };

    // Everything RendererAPI::GetPipelineReadiness reports.
    struct PipelineReadinessSnapshot
    {
        PipelineReadinessModel Model = PipelineReadinessModel::None;
        ShaderPreparationStats Shaders;

        // --- Vulkan pipeline objects (Model == VulkanPipelineObjects) --------
        PipelineRequestCounters Pipelines;
        bool PrecacheEnabled = false;
        FString ConfigurationTag;                              ///< The selected configuration requests are prepared for.
        u32 JobsPerFrame = 0;                                  ///< The creation budget handed to the worker each frame.
        u64 Queued = 0;                                        ///< Requests waiting for budget (carried over).
        u64 InFlight = 0;                                      ///< Creations running now.
        u64 CachedPipelines = 0;                               ///< Pipelines live in the builder.
        u64 ManifestEntries = 0;                               ///< Keys the manifest knows, all shaders and configurations.
        u32 TrackedShaders = 0;                                ///< Live shaders the collector requests for.
        u32 ReadyShaders = 0;                                  ///< Of those, every selected-configuration request prepared.
        u32 UnknownShaders = 0;                                ///< Live shaders the manifest has no entry for yet (first draw is a miss).
        bool RenderReady = false;                              ///< Every tracked shader with requests is ready and nothing is queued.
        TArray<PipelineReadinessPendingShader> PendingShaders; ///< Capped list of the not-ready ones.
    };
} // namespace OloEngine
