#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/RendererAPI.h" // ParallelRecordingFrameStats (issue #806)
#include <imgui.h>
#include <string>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include <unordered_map>
#include <chrono>
#include <array>

namespace OloEngine
{
    struct RendererProfilerFrameData
    {
        f64 m_FrameTime = 0.0;
        f64 m_CPUTime = 0.0;
        f64 m_GPUTime = 0.0;
        f64 m_GPUWaitTime = 0.0;
        u32 m_DrawCalls = 0;
        u32 m_StateChanges = 0;
        u32 m_ShaderBinds = 0;
        u32 m_TextureBinds = 0;
        u32 m_BufferBinds = 0;
        u32 m_VerticesRendered = 0;
        u32 m_TrianglesRendered = 0;
        u32 m_CommandPackets = 0;
        f64 m_SortingTime = 0.0;
        f64 m_CullingTime = 0.0;
        u32 m_InstancedDrawCalls = 0;
        u32 m_InstancesRendered = 0;
        u32 m_InstancesBatched = 0;
        GPUSceneFrameStats m_GPUScene;
        // This frame's GPU Scene draw links (issue #994), in two pairs
        // because they answer different questions.
        //
        // Resolved/unresolved is EXTRACTION health: how many links found a
        // committed record. Consumed/fallback is CONSUMPTION: how many
        // draws actually rendered through one. They are not the same
        // number — a resolved link is dropped when CommandBucket
        // auto-batches the draw, and a draw submitted to several passes
        // consumes once per pass. A migrated path that silently stops
        // consuming records still renders correctly, so the consumption
        // pair is what has to be visible for the migration to stay
        // migrated.
        u32 m_GPUSceneLinkedDraws = 0;
        u32 m_GPUSceneUnlinkedDraws = 0;
        u32 m_GPUSceneConsumedDraws = 0;
        u32 m_GPUSceneFallbackDraws = 0;
        // The parallel command recorder's telemetry for this frame (issue
        // #806, ADR 0011 amendment (92)). Pulled from the backend once per
        // frame in EndFrame(). All zero on OpenGL, whose facade default
        // reports nothing; on Vulkan with OLO_VK_PARALLEL_RECORDING off
        // only InlineRegions counts.
        RendererAPI::ParallelRecordingFrameStats m_ParallelRecording;
        // The async-compute queue's telemetry for this frame (issue #808),
        // pulled at the same point and for the same reason. All zero on
        // OpenGL and on a Vulkan device with no compute-only queue family;
        // there, BatchesDeclined counts the batches that stayed on the
        // graphics queue and DeclineReason says why.
        RendererAPI::AsyncComputeFrameStats m_AsyncCompute;

        void Reset();
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerFrameData>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_FrameTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_CPUTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_GPUTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_GPUWaitTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_DrawCalls)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_StateChanges)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_ShaderBinds)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_TextureBinds)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_BufferBinds)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_VerticesRendered)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_TrianglesRendered)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_CommandPackets)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_SortingTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_CullingTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_InstancedDrawCalls)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_InstancesRendered)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_InstancesBatched)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_GPUScene)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_GPUSceneLinkedDraws)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_GPUSceneUnlinkedDraws)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_GPUSceneConsumedDraws)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_GPUSceneFallbackDraws)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_ParallelRecording)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerFrameData::m_AsyncCompute)>::Value;
    };

    struct RendererProfilerBottleneckInfo
    {
        enum Type
        {
            CPU_Bound,
            GPU_Bound,
            Memory_Bound,
            IO_Bound,
            Balanced
        } m_Type;
        f32 m_Confidence = 0.0f; // 0.0 to 1.0
        FString m_Description;
        TArray<FString> m_Recommendations;
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerBottleneckInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerBottleneckInfo::m_Type)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerBottleneckInfo::m_Confidence)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerBottleneckInfo::m_Description)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerBottleneckInfo::m_Recommendations)>::Value;
    };

    struct RendererProfilerDrawCallInfo
    {
        FString m_Name;
        FString m_ShaderName;
        u32 m_VertexCount = 0;
        u32 m_IndexCount = 0;
        f64 m_CPUTime = 0.0;
        f64 m_GPUTime = 0.0;
        sizet m_TextureMemory = 0;
        sizet m_BufferMemory = 0;
        bool m_IsCulled = false;
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerDrawCallInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_Name)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_ShaderName)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_VertexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_IndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_CPUTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_GPUTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_TextureMemory)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_BufferMemory)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerDrawCallInfo::m_IsCulled)>::Value;
    };

    struct RendererProfilerInstancedDrawRecord
    {
        u64 m_MeshHandle = 0;
        u32 m_VertexArrayID = 0;
        u32 m_IndexCount = 0;
        u32 m_InstanceCount = 0;
        TArray<i32> m_EntityIDs;         // Per-instance source entity IDs (size == m_InstanceCount when populated).
        bool m_FromAutoBatching = false; // true: collapsed by CommandBucket from N DrawMeshCommands;
                                         // false: explicit InstancedMeshComponent submission.
        // Free-form label for *which* renderer pipeline emitted this
        // draw — lets the UI / clipboard report distinguish "Scene"
        // (main CommandDispatch::DrawMeshInstanced) from "Scene (GPU
        // cull)" (the indirect-draw branch) or "Shadow CSM cascade 0"
        // (ShadowRenderPass auto-batched casters). Defaults to "Scene"
        // so the existing call sites stay unchanged.
        FString m_Source = "Scene";
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerInstancedDrawRecord>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerInstancedDrawRecord::m_MeshHandle)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerInstancedDrawRecord::m_VertexArrayID)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerInstancedDrawRecord::m_IndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerInstancedDrawRecord::m_InstanceCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerInstancedDrawRecord::m_EntityIDs)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerInstancedDrawRecord::m_FromAutoBatching)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerInstancedDrawRecord::m_Source)>::Value;
    };

    struct RendererProfilerRenderPassInfo
    {
        FString m_Name;
        f64 m_StartTime = 0.0;
        f64 m_Duration = 0.0;
        u32 m_DrawCallCount = 0;
        TArray<RendererProfilerDrawCallInfo> m_DrawCalls;
        sizet m_MemoryUsed = 0;
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerRenderPassInfo>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerRenderPassInfo::m_Name)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerRenderPassInfo::m_StartTime)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerRenderPassInfo::m_Duration)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerRenderPassInfo::m_DrawCallCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerRenderPassInfo::m_DrawCalls)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerRenderPassInfo::m_MemoryUsed)>::Value;
    };

    struct RendererProfilerCapturedFrame
    {
        u32 m_FrameNumber = 0;
        f64 m_Timestamp = 0.0;
        RendererProfilerFrameData m_FrameData;
        TArray<RendererProfilerRenderPassInfo> m_RenderPasses;
        RendererProfilerBottleneckInfo m_BottleneckAnalysis;
        FString m_Notes; // User can add notes about why this frame was captured
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerCapturedFrame>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerCapturedFrame::m_FrameNumber)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerCapturedFrame::m_Timestamp)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerCapturedFrame::m_FrameData)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerCapturedFrame::m_RenderPasses)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerCapturedFrame::m_BottleneckAnalysis)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerCapturedFrame::m_Notes)>::Value;
    };

    struct RendererProfilerOptimizationPriority
    {
        enum Severity
        {
            Critical,
            High,
            Medium,
            Low
        };
        Severity m_Severity;
        FString m_Issue;
        FString m_Solution;
        f32 m_ExpectedGain; // Estimated FPS improvement
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerOptimizationPriority>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerOptimizationPriority::m_Severity)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerOptimizationPriority::m_Issue)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerOptimizationPriority::m_Solution)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerOptimizationPriority::m_ExpectedGain)>::Value;
    };

    struct RendererProfilerShipReadinessReport
    {
        f32 m_OverallScore; // 0-100
        bool m_ReadyForShipping;
        TArray<FString> m_CriticalIssues;
        TArray<FString> m_Recommendations;
        f32 m_AverageFrameRate;
        f32 m_WorstCaseFrameRate;
    };

    // Each owned allocation moves independently; records hold no interior pointers.
    template<>
    struct TIsTriviallyRelocatable<RendererProfilerShipReadinessReport>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(RendererProfilerShipReadinessReport::m_OverallScore)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerShipReadinessReport::m_ReadyForShipping)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerShipReadinessReport::m_CriticalIssues)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerShipReadinessReport::m_Recommendations)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerShipReadinessReport::m_AverageFrameRate)>::Value &&
                                      TIsTriviallyRelocatable<decltype(RendererProfilerShipReadinessReport::m_WorstCaseFrameRate)>::Value;
    };

    // @brief Performance profiler specifically for renderer operations
    //
    // Tracks frame timing, draw calls, state changes, and provides
    // detailed performance analysis for game developers.
    class RendererProfiler
    {
      public:
        // Performance metrics categories
        enum class MetricType : u8
        {
            FrameTime = 0,
            CPUTime,
            GPUTime,
            GPUWaitTime, // CPU time spent blocked on GPU fences (glClientWaitSync) — the direct "GPU-bound" signal
            DrawCalls,
            StateChanges,
            ShaderBinds,
            TextureBinds,
            BufferBinds,
            VerticesRendered,
            TrianglesRendered,
            CommandPackets,
            SortingTime,
            CullingTime,
            InstancedDrawCalls, // glDrawElementsInstanced calls this frame
            InstancesRendered,  // sum of all instances rendered across InstancedDrawCalls
            InstancesBatched,   // instances collapsed by CommandBucket auto-batching (visible savings vs naive submission)
            COUNT
        };

        // Timing scope for automatic profiling
        class ProfileScope
        {
          public:
            ProfileScope(const std::string& name, MetricType type = MetricType::CPUTime);
            ~ProfileScope();

          private:
            FString m_Name;
            MetricType m_Type;
            std::chrono::high_resolution_clock::time_point m_StartTime;
        };
        // Performance counter for custom metrics
        struct PerformanceCounter
        {
            f64 m_Value = 0.0;
            f64 m_Min = DBL_MAX;
            f64 m_Max = 0.0;
            f64 m_Average = 0.0;
            u32 m_SampleCount = 0;

            // Ring buffer for history (2 seconds at 60fps)
            static constexpr u32 OLO_HISTORY_SIZE = 120;
            TArray<f32> m_History;
            u32 m_HistoryIndex = 0;
            u32 m_HistoryCount = 0; // Tracks how many valid samples we have

            void AddSample(f64 value);
            void Reset();

            // Helper method to get history in chronological order (oldest to newest)
            // Useful for display purposes like ImGui::PlotLines
            void GetHistoryInOrder(TArray<f32>& outHistory) const;
        };

        // Frame performance data
        using FrameData = RendererProfilerFrameData;

        // Bottleneck analysis
        using BottleneckInfo = RendererProfilerBottleneckInfo;

        // Frame capture for detailed analysis
        using DrawCallInfo = RendererProfilerDrawCallInfo;

        // Per-draw record of an instanced submission — what mesh, how many
        // instances, which entity IDs collapsed into that single call. Populated
        // by the dispatcher when `m_RecordInstancedDraws` is on and cleared at
        // BeginFrame. Lets the UI answer "which entities batched together this
        // frame?" without touching the heavier frame-capture state machine.
        using InstancedDrawRecord = RendererProfilerInstancedDrawRecord;

        using RenderPassInfo = RendererProfilerRenderPassInfo;

        // Workers only append to their own record. The render thread publishes
        // counters and records in item order after every worker has joined.
        struct RecordingStats
        {
            std::array<u32, static_cast<sizet>(MetricType::COUNT)> Counters{};
            TArray<InstancedDrawRecord> InstancedDraws;
            void Reset();
            void Publish();
        };

        class ScopedRecordingStats
        {
          public:
            explicit ScopedRecordingStats(RecordingStats& stats);
            ~ScopedRecordingStats();
            ScopedRecordingStats(const ScopedRecordingStats&) = delete;
            ScopedRecordingStats& operator=(const ScopedRecordingStats&) = delete;

          private:
            RecordingStats* m_Previous;
        };

        using CapturedFrame = RendererProfilerCapturedFrame;

      public:
        static RendererProfiler& GetInstance();

        // @brief Initialize the profiler
        void Initialize();

        // @brief Shutdown the profiler
        void Shutdown();

        // @brief Reset all profiling data and statistics
        void Reset();

        // @brief Begin a new frame
        void BeginFrame();

        // @brief End the current frame and process metrics
        void EndFrame();

        // @brief Add a timing sample
        void AddTimingSample(const std::string& name, f64 timeMs, MetricType type = MetricType::CPUTime);

        // @brief Increment a counter metric
        void IncrementCounter(MetricType type, u32 value = 1);

        // @brief Set a value metric
        void SetValue(MetricType type, f64 value);

        void SetGPUSceneStats(const GPUSceneFrameStats& stats)
        {
            m_CurrentFrame.m_GPUScene = stats;
        }

        void SetGPUSceneDrawLinkCounts(u32 linked, u32 unlinked)
        {
            m_CurrentFrame.m_GPUSceneLinkedDraws = linked;
            m_CurrentFrame.m_GPUSceneUnlinkedDraws = unlinked;
        }

        void SetGPUSceneConsumptionCounts(u32 consumed, u32 fallback)
        {
            m_CurrentFrame.m_GPUSceneConsumedDraws = consumed;
            m_CurrentFrame.m_GPUSceneFallbackDraws = fallback;
        }

        // @brief Accumulate CPU time spent blocked on a GPU fence this frame.
        // EndFrame() subtracts the total from m_CPUTime so cpu time reflects
        // actual CPU work, and publishes it as the GPUWaitTime metric.
        void AddGPUWaitTime(f64 timeMs)
        {
            m_CurrentFrame.m_GPUWaitTime += timeMs;
        }

        // @brief Accumulate CPU time spent blocked on GPU/present sync that
        // happens AFTER this frame's EndFrame() has already run — e.g. the
        // SwapBuffers() call, which can block on vsync or on the driver
        // throttling a GPU-bound app. EndFrame() has already finalized this
        // frame's CPUTime/GPUWaitTime/history entry by the time SwapBuffers
        // returns, so the wait recorded here is patched into that
        // already-recorded frame at the START of the NEXT BeginFrame() (the
        // only point where it's known) instead of being silently dropped.
        // See BeginFrame() for why. Fixes gpuWaitMs reading ~0 while clearly
        // GPU-bound (#519).
        void AddPostFrameGPUWaitTime(f64 timeMs)
        {
            m_PendingPostFrameGPUWaitTime += timeMs;
        }

        // @brief Render the profiler UI
        void RenderUI(bool* open = nullptr);

        // @brief Get current (in-progress) frame data. FrameTime here is a
        // LIVE ESTIMATE carried over from the previous frame's measurement —
        // fine for a live-updating UI, but do NOT use this for a snapshot
        // that must be internally self-consistent (its CPUTime/GPUTime
        // describe the in-progress frame while FrameTime describes the
        // previous one). Use GetLastCompletedFrameData() for that.
        const FrameData& GetCurrentFrameData() const
        {
            return m_CurrentFrame;
        }

        // @brief Get the most recently fully-finalized frame: FrameTime,
        // CPUTime, GPUTime and GPUWaitTime all describe the SAME completed
        // frame (unlike GetCurrentFrameData(), whose FrameTime is only a
        // live estimate — see there). Lags the in-progress frame by up to
        // one frame. Use this for any external snapshot that must not mix
        // measurements from different frames, e.g. the MCP perf tools (#519).
        const FrameData& GetLastCompletedFrameData() const
        {
            return m_LastCompletedFrame;
        }

        // @brief Get performance counter
        const PerformanceCounter& GetCounter(MetricType type) const;

        // @brief Analyze performance bottlenecks
        BottleneckInfo AnalyzeBottlenecks() const;

        // Frame Capture & Analysis Methods
        // @brief Capture the current frame for detailed analysis
        void CaptureFrame(const std::string& notes = "");

        // @brief Begin tracking a render pass
        void BeginRenderPass(const std::string& passName);

        // @brief End tracking a render pass
        void EndRenderPass();

        // @brief Track a draw call within the current render pass
        void TrackDrawCall(const std::string& name, const std::string& shaderName,
                           u32 vertexCount, u32 indexCount, f64 cpuTime, f64 gpuTime = 0.0);

        // @brief Record an instanced draw with its source entity IDs.
        //
        // Cheap when m_RecordInstancedDraws is false (default) — single bool
        // check and early-out. Toggle on via the profiler UI when the user
        // wants to see "which entities collapsed into which draw call". Each
        // record is held until the next BeginFrame() then discarded.
        void RecordInstancedDraw(u64 meshHandle, u32 vertexArrayID, u32 indexCount,
                                 u32 instanceCount, const i32* entityIDs, bool fromAutoBatching,
                                 const char* source = "Scene");

        // @brief Toggle per-frame instanced-draw recording.
        void SetRecordInstancedDraws(bool enabled)
        {
            m_RecordInstancedDraws = enabled;
        }
        [[nodiscard]] bool IsRecordingInstancedDraws() const
        {
            return m_RecordInstancedDraws;
        }
        [[nodiscard]] const TArray<InstancedDrawRecord>& GetInstancedDrawRecords() const
        {
            return m_InstancedDrawRecords;
        }

        // @brief Get captured frames for analysis
        const TArray<CapturedFrame>& GetCapturedFrames() const
        {
            return m_CapturedFrames;
        }

        // @brief Copy of the per-frame ring buffer in chronological (oldest-first)
        // order. m_FrameHistory is a ring indexed by m_HistoryIndex (the next write
        // slot, i.e. the oldest entry), so rotate it the same way RenderUI does.
        // Returns a copy so callers reading off the render thread (e.g. the MCP
        // diagnostics server, #285, via a main-thread marshal) get a stable snapshot.
        [[nodiscard]] TArray<FrameData> GetFrameHistoryCopy() const
        {
            const std::size_t n = m_FrameHistory.Num();
            TArray<FrameData> ordered;
            ordered.Reserve(static_cast<i32>(n));
            for (std::size_t i = 0; i < n; ++i)
                ordered.Add(m_FrameHistory[(m_HistoryIndex + i) % n]);
            return ordered;
        }

        // @brief Clear captured frames
        void ClearCapturedFrames()
        {
            m_CapturedFrames.Reset();
        }

        // @brief Compare two captured frames
        FString CompareFrames(const CapturedFrame& frame1, const CapturedFrame& frame2) const;

        // @brief Export performance data to CSV
        bool ExportToCSV(const std::string& filePath) const;

        // @brief Check if we're hitting target framerate
        bool IsHittingTargetFramerate(f32 targetFPS = 60.0f) const;

        // @brief Get performance health score (0-100)
        f32 GetPerformanceHealthScore() const;

        // @brief Get optimization priority list for game developers
        using OptimizationPriority = RendererProfilerOptimizationPriority;
        TArray<OptimizationPriority> GetOptimizationPriorities() const;

        // @brief Generate ship readiness report
        using ShipReadinessReport = RendererProfilerShipReadinessReport;
        ShipReadinessReport GenerateShipReadinessReport() const;

      private:
        RendererProfiler() = default;
        ~RendererProfiler() = default;
        // UI rendering methods
        void RenderOverviewTab() const;
        void RenderDetailedTimingTab() const;
        void RenderBottleneckAnalysisTab() const;
        void RenderCountersTab();
        void RenderHistoryTab();
        void RenderFrameCaptureTab();
        void RenderFrameComparisonTab();
        void RenderInstancedDrawsTab();

        // Helper methods
        std::string_view GetMetricTypeName(MetricType type) const;
        std::string_view GetMetricTypeUnit(MetricType type) const;
        ImVec4 GetMetricTypeColor(MetricType type) const;
        f32 CalculateFrameRate() const;
        f32 CalculateAverageFrameTime() const;
        // Frame capture state
        TArray<CapturedFrame> m_CapturedFrames;
        bool m_CapturingFrame = false;
        RenderPassInfo* m_CurrentRenderPass = nullptr;
        u32 m_FrameNumber = 0;
        static constexpr u32 OLO_MAX_CAPTURED_FRAMES = 10; // Keep only last 10 captured frames

        // Instanced-draw breakdown (opt-in, per-frame). When
        // m_RecordInstancedDraws is true, the dispatcher pushes one record
        // here per glDrawElementsInstanced — the UI then shows the entity ID
        // list per call so authors can see exactly which entities batched
        // together. Cleared every BeginFrame() to bound memory.
        TArray<InstancedDrawRecord> m_InstancedDrawRecords;
        bool m_RecordInstancedDraws = false;

        // Data storage
        FrameData m_CurrentFrame;
        FrameData m_PreviousFrame;      // raw EndFrame() output for the last frame; FrameTime/GPUWaitTime unpatched until the next BeginFrame()
        FrameData m_LastCompletedFrame; // fully patched, self-consistent snapshot — see GetLastCompletedFrameData()
        std::unordered_map<MetricType, PerformanceCounter> m_Counters;
        std::unordered_map<std::string, PerformanceCounter> m_CustomTimings;

        // History tracking
        static constexpr u32 OLO_FRAME_HISTORY_SIZE = 300; // 5 seconds at 60fps
        TArray<FrameData> m_FrameHistory;
        u32 m_HistoryIndex = 0;
        u32 m_LastWrittenHistoryIndex = 0; // slot EndFrame() last wrote; patched in place by the next BeginFrame()

        // Frame timing
        std::chrono::high_resolution_clock::time_point m_FrameStartTime;
        std::chrono::high_resolution_clock::time_point m_LastFrameTime;
        bool m_HasCompletedFrame = false;        // true once at least one EndFrame() has run — guards the BeginFrame() patch step
        f64 m_PendingPostFrameGPUWaitTime = 0.0; // accumulated via AddPostFrameGPUWaitTime() since the last EndFrame()

        // Configuration
        f32 m_TargetFrameRate = 60.0f;
        bool m_EnableGPUTiming = true; // GPUPassTimerPool feeds GPUTime every frame; toggle only hides the UI plot
        bool m_ShowAdvancedMetrics = false;
        bool m_AutoAnalyzeBottlenecks = true;
        // UI state
        i32 m_SelectedTab = 0;
        bool m_PauseUpdates = false;
        f32 m_UpdateInterval = 1.0f / 60.0f;
        f64 m_LastUpdateTime = 0.0;
    };

// Convenience macro for automatic scope timing
#define OLO_PROFILE_RENDERER_SCOPE(name) \
    RendererProfiler::ProfileScope _profileScope(name, RendererProfiler::MetricType::CPUTime)

#define OLO_PROFILE_RENDERER_GPU_SCOPE(name) \
    RendererProfiler::ProfileScope _profileScope(name, RendererProfiler::MetricType::GPUTime)
} // namespace OloEngine
