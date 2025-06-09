#pragma once

#include "OloEngine/Core/Base.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace OloEngine
{
    /**
     * @brief Performance profiler specifically for renderer operations
     * 
     * Tracks frame timing, draw calls, state changes, and provides
     * detailed performance analysis for game developers.
     */
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
            std::vector<f32> m_History;
            
            void AddSample(f64 value);
            void Reset();
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
            
            void Reset();
        };
        
        // Bottleneck analysis
        struct BottleneckInfo
        {
            enum Type { CPU_Bound, GPU_Bound, Memory_Bound, IO_Bound, Balanced } m_Type;
            f32 m_Confidence = 0.0f; // 0.0 to 1.0
            std::string m_Description;
            std::vector<std::string> m_Recommendations;
        };
        
    public:
        static RendererProfiler& GetInstance();
        
        /**
         * @brief Initialize the profiler
         */
        void Initialize();
        
        /**
         * @brief Shutdown the profiler
         */
        void Shutdown();
        
        /**
         * @brief Reset all profiling data and statistics
         */
        void Reset();
        
        /**
         * @brief Begin a new frame
         */
        void BeginFrame();
        
        /**
         * @brief End the current frame and process metrics
         */
        void EndFrame();
        
        /**
         * @brief Add a timing sample
         */
        void AddTimingSample(const std::string& name, f64 timeMs, MetricType type = MetricType::CPUTime);
        
        /**
         * @brief Increment a counter metric
         */
        void IncrementCounter(MetricType type, u32 value = 1);
        
        /**
         * @brief Set a value metric
         */
        void SetValue(MetricType type, f64 value);
        
        /**
         * @brief Render the profiler UI
         */
        void RenderUI(bool* open = nullptr);
        
        /**
         * @brief Get current frame data
         */
        const FrameData& GetCurrentFrameData() const { return m_CurrentFrame; }
        
        /**
         * @brief Get performance counter
         */
        const PerformanceCounter& GetCounter(MetricType type) const;
        
        /**
         * @brief Analyze performance bottlenecks
         */
        BottleneckInfo AnalyzeBottlenecks() const;
        
        /**
         * @brief Export performance data to CSV
         */
        bool ExportToCSV(const std::string& filePath) const;
        
        /**
         * @brief Check if we're hitting target framerate
         */
        bool IsHittingTargetFramerate(f32 targetFPS = 60.0f) const;
        
    private:
        RendererProfiler() = default;
        ~RendererProfiler() = default;
        
        // UI rendering methods
        void RenderOverviewTab();
        void RenderDetailedTimingTab();
        void RenderBottleneckAnalysisTab();
        void RenderCountersTab();
        void RenderHistoryTab();
        
        // Helper methods
        std::string GetMetricTypeName(MetricType type) const;
        std::string GetMetricTypeUnit(MetricType type) const;
        ImVec4 GetMetricTypeColor(MetricType type) const;
        f32 CalculateFrameRate() const;
        f32 CalculateAverageFrameTime() const;
        
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
}
