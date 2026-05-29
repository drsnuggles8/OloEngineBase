#pragma once

#include "OloEngine/Core/Base.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace OloEngine
{
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
            std::string m_Name;
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
            std::vector<f32> m_History;
            u32 m_HistoryIndex = 0;
            u32 m_HistoryCount = 0; // Tracks how many valid samples we have

            void AddSample(f64 value);
            void Reset();

            // Helper method to get history in chronological order (oldest to newest)
            // Useful for display purposes like ImGui::PlotLines
            void GetHistoryInOrder(std::vector<f32>& outHistory) const;
        };

        // Frame performance data
        struct FrameData
        {
            f64 m_FrameTime = 0.0;
            f64 m_CPUTime = 0.0;
            f64 m_GPUTime = 0.0;
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

            void Reset();
        };

        // Bottleneck analysis
        struct BottleneckInfo
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
            std::string m_Description;
            std::vector<std::string> m_Recommendations;
        };

        // Frame capture for detailed analysis
        struct DrawCallInfo
        {
            std::string m_Name;
            std::string m_ShaderName;
            u32 m_VertexCount = 0;
            u32 m_IndexCount = 0;
            f64 m_CPUTime = 0.0;
            f64 m_GPUTime = 0.0;
            sizet m_TextureMemory = 0;
            sizet m_BufferMemory = 0;
            bool m_IsCulled = false;
        };

        // Per-draw record of an instanced submission — what mesh, how many
        // instances, which entity IDs collapsed into that single call. Populated
        // by the dispatcher when `m_RecordInstancedDraws` is on and cleared at
        // BeginFrame. Lets the UI answer "which entities batched together this
        // frame?" without touching the heavier frame-capture state machine.
        struct InstancedDrawRecord
        {
            u64 m_MeshHandle = 0;
            u32 m_VertexArrayID = 0;
            u32 m_IndexCount = 0;
            u32 m_InstanceCount = 0;
            std::vector<i32> m_EntityIDs;    // Per-instance source entity IDs (size == m_InstanceCount when populated).
            bool m_FromAutoBatching = false; // true: collapsed by CommandBucket from N DrawMeshCommands;
                                             // false: explicit InstancedMeshComponent submission.
            // Free-form label for *which* renderer pipeline emitted this
            // draw — lets the UI / clipboard report distinguish "Scene"
            // (main CommandDispatch::DrawMeshInstanced) from "Scene (GPU
            // cull)" (the indirect-draw branch) or "Shadow CSM cascade 0"
            // (ShadowRenderPass auto-batched casters). Defaults to "Scene"
            // so the existing call sites stay unchanged.
            std::string m_Source = "Scene";
        };

        struct RenderPassInfo
        {
            std::string m_Name;
            f64 m_StartTime = 0.0;
            f64 m_Duration = 0.0;
            u32 m_DrawCallCount = 0;
            std::vector<DrawCallInfo> m_DrawCalls;
            sizet m_MemoryUsed = 0;
        };

        struct CapturedFrame
        {
            u32 m_FrameNumber = 0;
            f64 m_Timestamp = 0.0;
            FrameData m_FrameData;
            std::vector<RenderPassInfo> m_RenderPasses;
            BottleneckInfo m_BottleneckAnalysis;
            std::string m_Notes; // User can add notes about why this frame was captured
        };

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

        // @brief Render the profiler UI
        void RenderUI(bool* open = nullptr);

        // @brief Get current frame data
        const FrameData& GetCurrentFrameData() const
        {
            return m_CurrentFrame;
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
        [[nodiscard]] const std::vector<InstancedDrawRecord>& GetInstancedDrawRecords() const
        {
            return m_InstancedDrawRecords;
        }

        // @brief Get captured frames for analysis
        const std::vector<CapturedFrame>& GetCapturedFrames() const
        {
            return m_CapturedFrames;
        }

        // @brief Clear captured frames
        void ClearCapturedFrames()
        {
            m_CapturedFrames.clear();
        }

        // @brief Compare two captured frames
        std::string CompareFrames(const CapturedFrame& frame1, const CapturedFrame& frame2) const;

        // @brief Export performance data to CSV
        bool ExportToCSV(const std::string& filePath) const;

        // @brief Check if we're hitting target framerate
        bool IsHittingTargetFramerate(f32 targetFPS = 60.0f) const;

        // @brief Get performance health score (0-100)
        f32 GetPerformanceHealthScore() const;

        // @brief Get optimization priority list for game developers
        struct OptimizationPriority
        {
            enum Severity
            {
                Critical,
                High,
                Medium,
                Low
            };
            Severity m_Severity;
            std::string m_Issue;
            std::string m_Solution;
            f32 m_ExpectedGain; // Estimated FPS improvement
        };
        std::vector<OptimizationPriority> GetOptimizationPriorities() const;

        // @brief Generate ship readiness report
        struct ShipReadinessReport
        {
            f32 m_OverallScore; // 0-100
            bool m_ReadyForShipping;
            std::vector<std::string> m_CriticalIssues;
            std::vector<std::string> m_Recommendations;
            f32 m_AverageFrameRate;
            f32 m_WorstCaseFrameRate;
        };
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
        std::string GetMetricTypeName(MetricType type) const;
        std::string GetMetricTypeUnit(MetricType type) const;
        ImVec4 GetMetricTypeColor(MetricType type) const;
        f32 CalculateFrameRate() const;
        f32 CalculateAverageFrameTime() const;
        // Frame capture state
        std::vector<CapturedFrame> m_CapturedFrames;
        bool m_CapturingFrame = false;
        RenderPassInfo* m_CurrentRenderPass = nullptr;
        u32 m_FrameNumber = 0;
        static constexpr u32 OLO_MAX_CAPTURED_FRAMES = 10; // Keep only last 10 captured frames

        // Instanced-draw breakdown (opt-in, per-frame). When
        // m_RecordInstancedDraws is true, the dispatcher pushes one record
        // here per glDrawElementsInstanced — the UI then shows the entity ID
        // list per call so authors can see exactly which entities batched
        // together. Cleared every BeginFrame() to bound memory.
        std::vector<InstancedDrawRecord> m_InstancedDrawRecords;
        bool m_RecordInstancedDraws = false;

        // Data storage
        FrameData m_CurrentFrame;
        FrameData m_PreviousFrame;
        std::unordered_map<MetricType, PerformanceCounter> m_Counters;
        std::unordered_map<std::string, PerformanceCounter> m_CustomTimings;

        // History tracking
        static constexpr u32 OLO_FRAME_HISTORY_SIZE = 300; // 5 seconds at 60fps
        std::vector<FrameData> m_FrameHistory;
        u32 m_HistoryIndex = 0;

        // Frame timing
        std::chrono::high_resolution_clock::time_point m_FrameStartTime;
        std::chrono::high_resolution_clock::time_point m_LastFrameTime;

        // Configuration
        f32 m_TargetFrameRate = 60.0f;
        bool m_EnableGPUTiming = false; // Requires GPU timing queries
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
