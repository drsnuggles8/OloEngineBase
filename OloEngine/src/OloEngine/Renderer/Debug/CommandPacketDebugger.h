#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"

#include <imgui.h>
#include <vector>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    /**
     * @brief Debug visualization tool for command packets and draw keys
     * 
     * Provides functionality to visualize command packets, their sorting keys,
     * memory usage, and performance metrics in the ImGui interface.
     */
    class CommandPacketDebugger
    {
    public:
        CommandPacketDebugger() = default;
        ~CommandPacketDebugger() = default;
        
        /**
         * @brief Renders a debug view of command packets in an ImGui window
         * 
         * @param bucket The command bucket to visualize
         * @param open Pointer to a boolean that controls the visibility of the window
         * @param title The title of the ImGui window
         */
        void RenderDebugView(const CommandBucket* bucket, bool* open = nullptr, const char* title = "Command Packet Debugger");
        
        /**
         * @brief Updates frame statistics for performance tracking
         */
        void UpdateFrameStats();
        
        /**
         * @brief Renders memory usage statistics
         */
        void RenderMemoryStats();
        
        /**
         * @brief Renders performance metrics
         */
        void RenderPerformanceStats();
        
        /**
         * @brief Renders detailed command packet list
         */
        void RenderCommandPacketList(const CommandBucket* bucket);
        
        /**
         * @brief Renders draw key analysis
         */
        void RenderDrawKeyAnalysis(const CommandBucket* bucket);
        
        /**
         * @brief Exports command packet data to CSV for external analysis
         * 
         * @param bucket The command bucket to export
         * @param outputPath The file path to save the CSV file
         * @return True if the export was successful, false otherwise
         */
        bool ExportToCSV(const CommandBucket* bucket, const std::string& outputPath) const;
        
    private:
        // Frame statistics tracking
        struct FrameStats
        {
            u32 m_TotalPackets = 0;
            u32 m_SortedPackets = 0;
            u32 m_StaticPackets = 0;
            u32 m_DynamicPackets = 0;
            u32 m_StateChanges = 0;
            f32 m_SortingTimeMs = 0.0f;
            f32 m_ExecutionTimeMs = 0.0f;
            
            void Reset()
            {
                m_TotalPackets = 0;
                m_SortedPackets = 0;
                m_StaticPackets = 0;
                m_DynamicPackets = 0;
                m_StateChanges = 0;
                m_SortingTimeMs = 0.0f;
                m_ExecutionTimeMs = 0.0f;
            }
        };
        
        // Memory usage tracking
        struct MemoryStats
        {
            size_t m_CommandPacketMemory = 0;
            size_t m_MetadataMemory = 0;
            size_t m_AllocatorMemory = 0;
            u32 m_AllocationCount = 0;
            u32 m_DeallocationCount = 0;
            
            void Reset()
            {
                m_CommandPacketMemory = 0;
                m_MetadataMemory = 0;
                m_AllocatorMemory = 0;
                m_AllocationCount = 0;
                m_DeallocationCount = 0;
            }
        };
        
        // Draw key analysis data
        struct DrawKeyStats
        {
            std::unordered_map<u32, u32> m_LayerDistribution;
            std::unordered_map<u32, u32> m_MaterialDistribution;
            std::unordered_map<u32, u32> m_DepthDistribution;
            std::unordered_map<u32, u32> m_TranslucencyDistribution;
            
            void Reset()
            {
                m_LayerDistribution.clear();
                m_MaterialDistribution.clear();
                m_DepthDistribution.clear();
                m_TranslucencyDistribution.clear();
            }
        };
        
        // Helper methods
        void AnalyzeDrawKeys(const CommandBucket* bucket);
        void RenderDrawKeyHistogram(const std::unordered_map<u32, u32>& distribution, const char* label);
        std::string FormatMemorySize(size_t bytes) const;
        ImVec4 GetColorForPacketType(const CommandPacket* packet) const;
        std::string GetPacketTypeString(const CommandPacket* packet) const;
        
        // Configuration
        bool m_ShowMemoryStats = true;
        bool m_ShowPerformanceStats = true;
        bool m_ShowCommandList = true;
        bool m_ShowDrawKeyAnalysis = true;
        bool m_AutoRefresh = true;
        f32 m_RefreshRate = 60.0f; // Hz
        
        // Data
        FrameStats m_CurrentFrameStats;
        FrameStats m_PreviousFrameStats;
        MemoryStats m_MemoryStats;
        DrawKeyStats m_DrawKeyStats;
        
        // History for graphs
        static constexpr u32 OLO_HISTORY_SIZE = 120; // 2 seconds at 60fps
        std::vector<f32> m_PacketCountHistory;
        std::vector<f32> m_SortingTimeHistory;
        std::vector<f32> m_ExecutionTimeHistory;
        std::vector<f32> m_MemoryUsageHistory;
        u32 m_HistoryIndex = 0;
        
        // UI state
        i32 m_SelectedPacketIndex = -1;
        bool m_FilterByType = false;
        i32 m_TypeFilter = 0;
        bool m_FilterByStatic = false;
        bool m_StaticFilter = true;
    };
}
