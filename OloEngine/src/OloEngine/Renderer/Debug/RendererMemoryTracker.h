#pragma once

#include "OloEngine/Core/Base.h"
#include "DebugUtils.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>
#include <array>

namespace OloEngine
{
    /**
     * @brief Real-time memory usage tracker for renderer resources
     * 
     * Tracks GPU and CPU memory allocations, provides leak detection,
     * and offers detailed memory usage visualization.
     */
    class RendererMemoryTracker
    {
    public:
        // Resource types for categorization
        enum class ResourceType : u8
        {
            VertexBuffer = 0,
            IndexBuffer,
            UniformBuffer,
            Texture2D,
            TextureCubemap,
            Framebuffer,
            Shader,
            RenderTarget,
            CommandBuffer,
            Other,
            COUNT
        };
        
        // Memory allocation info
        struct AllocationInfo
        {
            void* m_Address = nullptr;
            sizet m_Size = 0;
            ResourceType m_Type = ResourceType::Other;
            std::string m_Name;
            std::string m_File;
            u32 m_Line = 0;
            f64 m_Timestamp = 0.0;
            bool m_IsGPU = false;
        };
        
        // Memory pool statistics
        struct PoolStats
        {
            sizet m_TotalSize = 0;
            sizet m_UsedSize = 0;
            sizet m_FreeSize = 0;
            u32 m_AllocationCount = 0;
            f32 m_FragmentationPercentage = 0.0f;
        };
        
        // Memory leak detection
        struct LeakInfo
        {
            AllocationInfo m_Allocation;
            f64 m_AgeSeconds = 0.0;
            bool m_IsSuspicious = false;
        };
        
    public:
        static RendererMemoryTracker& GetInstance();
        
        // Debug function to track when memory gets corrupted
        void DebugDumpTypeUsage(const std::string& context);
        
        /**
         * @brief Initialize the memory tracker
         */
        void Initialize();
        
        /**
         * @brief Shutdown the memory tracker
         */
        void Shutdown();
        
        /**
         * @brief Reset all tracking data and statistics
         */
        void Reset();
        
        /**
         * @brief Track a memory allocation
         */
        void TrackAllocation(void* address, sizet size, ResourceType type, 
                           const std::string& name, bool isGPU = false,
                           const char* file = __FILE__, u32 line = __LINE__);
        
        /**
         * @brief Track a memory deallocation
         */
        void TrackDeallocation(void* address);
        
        /**
         * @brief Update memory statistics (call once per frame)
         */
        void UpdateStats();
        
        /**
         * @brief Render the memory tracker UI
         */
        void RenderUI(bool* open = nullptr);
        
        /**
         * @brief Get current memory usage by type
         */
        sizet GetMemoryUsage(ResourceType type) const;
        
        /**
         * @brief Get total memory usage
         */
        sizet GetTotalMemoryUsage() const;
        
        /**
         * @brief Get allocation count by type
         */
        u32 GetAllocationCount(ResourceType type) const;
        
        /**
         * @brief Detect potential memory leaks
         */
        std::vector<LeakInfo> DetectLeaks() const;
        
        /**
         * @brief Export memory report to file
         */
        bool ExportReport(const std::string& filePath) const;
        
    private:
        RendererMemoryTracker() = default;
        ~RendererMemoryTracker() = default;
          // Helper methods
        void RenderOverviewTab();        void RenderDetailedTab();
        void RenderLeakDetectionTab();
        void RenderPoolStatsTab();
        void RenderHistoryGraphs();
        
        std::string GetResourceTypeName(ResourceType type) const;
        ImVec4 GetResourceTypeColor(ResourceType type) const;
        
        // Internal helper (assumes lock is already held)
        sizet GetTotalMemoryUsageUnlocked() const;
        
        // Thread safety
        mutable std::mutex m_Mutex;
          // Allocation tracking
        std::unordered_map<void*, AllocationInfo> m_Allocations;
        std::array<sizet, static_cast<sizet>(ResourceType::COUNT)> m_TypeUsage{};
        std::array<u32, static_cast<sizet>(ResourceType::COUNT)> m_TypeCounts{};
        
        // History for graphs
        static constexpr u32 OLO_HISTORY_SIZE = 300; // 5 minutes at 60fps
        std::vector<f32> m_MemoryHistory;
        std::vector<f32> m_AllocationHistory;
        std::vector<f32> m_GPUMemoryHistory;
        std::vector<f32> m_CPUMemoryHistory;
        u32 m_HistoryIndex = 0;
        
        // Pool statistics (placeholder for future implementation)
        std::unordered_map<std::string, PoolStats> m_PoolStats;
          // Leak detection parameters
        f64 m_LeakDetectionThreshold = 30.0; // seconds
        f64 m_LastLeakCheck = 0.0;
        
        // UI state
        i32 m_SelectedTabIndex = 0;
        bool m_ShowSystemMemory = true;
        bool m_ShowDetailedView = false;
        bool m_EnableLeakDetection = true;
        f32 m_RefreshInterval = 1.0f / 60.0f; // 60 FPS
        
        // Statistics
        sizet m_PeakMemoryUsage = 0;
        sizet m_TotalAllocatedMemory = 0;
        sizet m_TotalDeallocatedMemory = 0;
        sizet m_CurrentMemoryUsage = 0;
        sizet m_TotalAllocations = 0;
        sizet m_TotalDeallocations = 0;        sizet m_CurrentAllocations = 0;
        sizet m_GPUMemoryUsage = 0;
        sizet m_CPUMemoryUsage = 0;        sizet m_PeakGPUMemory = 0;
        sizet m_PeakCPUMemory = 0;
        f64 m_LastUpdateTime = 0.0;
          // Shutdown tracking
        std::atomic<bool> m_IsShutdown{false};
        std::atomic<bool> m_IsInitialized{false};
    };
}

// Convenience macros for tracking allocations (defined outside namespace for global use)
#define OLO_TRACK_GPU_ALLOC(ptr, size, type, name) \
    do { \
        OloEngine::RendererMemoryTracker::GetInstance().TrackAllocation(ptr, size, type, name, true, __FILE__, __LINE__); \
    } while (0)

#define OLO_TRACK_CPU_ALLOC(ptr, size, type, name) \
    do { \
        OloEngine::RendererMemoryTracker::GetInstance().TrackAllocation(ptr, size, type, name, false, __FILE__, __LINE__); \
    } while (0)

#define OLO_TRACK_DEALLOC(ptr) \
    do { \
        OloEngine::RendererMemoryTracker::GetInstance().TrackDeallocation(ptr); \
    } while (0)
