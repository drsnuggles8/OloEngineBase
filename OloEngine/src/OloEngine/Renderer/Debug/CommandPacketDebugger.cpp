#include "CommandPacketDebugger.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
    // Helper function to get current time as seconds since epoch
    f64 GetCurrentTimeSeconds()
    {
        return std::chrono::duration<f64>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }
}

namespace OloEngine
{
    void CommandPacketDebugger::RenderDebugView(const CommandBucket* bucket, bool* open, const char* title)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!bucket)
        {
            if (open && *open)
            {
                ImGui::Begin(title, open);
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No command bucket available!");
                if (ImGui::Button("Close"))
                    *open = false;
                ImGui::End();
            }
            return;
        }
        
        if (!open || *open)
        {
            ImGui::Begin(title, open, ImGuiWindowFlags_MenuBar);
            
            // Menu bar with configuration options
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("View"))
                {
                    ImGui::MenuItem("Memory Stats", nullptr, &m_ShowMemoryStats);
                    ImGui::MenuItem("Performance Stats", nullptr, &m_ShowPerformanceStats);
                    ImGui::MenuItem("Command List", nullptr, &m_ShowCommandList);
                    ImGui::MenuItem("Draw Key Analysis", nullptr, &m_ShowDrawKeyAnalysis);
                    ImGui::EndMenu();
                }
                
                if (ImGui::BeginMenu("Options"))
                {
                    ImGui::MenuItem("Auto Refresh", nullptr, &m_AutoRefresh);
                    ImGui::SliderFloat("Refresh Rate", &m_RefreshRate, 1.0f, 120.0f, "%.1f Hz");
                    
                    ImGui::Separator();
                    if (ImGui::Button("Export to CSV"))
                    {
                        ExportToCSV(bucket, "command_packets_debug.csv");
                    }
                    
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            
            // Update frame stats if auto-refresh is enabled
            if (m_AutoRefresh)
            {
                static f32 s_LastUpdate = 0.0f;
                f32 currentTime = (f32)GetCurrentTimeSeconds();
                if (currentTime - s_LastUpdate >= (1.0f / m_RefreshRate))
                {
                    UpdateFrameStats();
                    s_LastUpdate = currentTime;
                }
            }
            
            // Analyze current frame data
            AnalyzeDrawKeys(bucket);
            
            // Render different sections based on configuration
            if (m_ShowMemoryStats)
            {
                if (ImGui::CollapsingHeader("Memory Usage", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RenderMemoryStats();
                }
            }
            
            if (m_ShowPerformanceStats)
            {
                if (ImGui::CollapsingHeader("Performance Metrics", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RenderPerformanceStats();
                }
            }
            
            if (m_ShowDrawKeyAnalysis)
            {
                if (ImGui::CollapsingHeader("Draw Key Analysis", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RenderDrawKeyAnalysis(bucket);
                }
            }
            
            if (m_ShowCommandList)
            {
                if (ImGui::CollapsingHeader("Command Packets", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RenderCommandPacketList(bucket);
                }
            }
            
            ImGui::End();
        }
    }
    
    void CommandPacketDebugger::UpdateFrameStats()
    {
        OLO_PROFILE_FUNCTION();
        
        // Move current stats to previous
        m_PreviousFrameStats = m_CurrentFrameStats;
        m_CurrentFrameStats.Reset();
        
        // Update history arrays
        if (m_PacketCountHistory.size() < OLO_HISTORY_SIZE)
        {
            m_PacketCountHistory.resize(OLO_HISTORY_SIZE, 0.0f);
            m_SortingTimeHistory.resize(OLO_HISTORY_SIZE, 0.0f);
            m_ExecutionTimeHistory.resize(OLO_HISTORY_SIZE, 0.0f);
            m_MemoryUsageHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        }
        
        m_PacketCountHistory[m_HistoryIndex] = (f32)m_CurrentFrameStats.m_TotalPackets;
        m_SortingTimeHistory[m_HistoryIndex] = m_CurrentFrameStats.m_SortingTimeMs;
        m_ExecutionTimeHistory[m_HistoryIndex] = m_CurrentFrameStats.m_ExecutionTimeMs;
        m_MemoryUsageHistory[m_HistoryIndex] = (f32)m_MemoryStats.m_CommandPacketMemory;
        
        m_HistoryIndex = (m_HistoryIndex + 1) % OLO_HISTORY_SIZE;
    }
    
    void CommandPacketDebugger::RenderMemoryStats()
    {
        OLO_PROFILE_FUNCTION();
        
        ImGui::Text("Command Packet Memory: %s", FormatMemorySize(m_MemoryStats.m_CommandPacketMemory).c_str());
        ImGui::Text("Metadata Memory: %s", FormatMemorySize(m_MemoryStats.m_MetadataMemory).c_str());
        ImGui::Text("Allocator Memory: %s", FormatMemorySize(m_MemoryStats.m_AllocatorMemory).c_str());
        
        ImGui::Separator();
        ImGui::Text("Allocations this frame: %u", m_MemoryStats.m_AllocationCount);
        ImGui::Text("Deallocations this frame: %u", m_MemoryStats.m_DeallocationCount);
        
        // Memory usage graph
        if (!m_MemoryUsageHistory.empty())
        {
            ImGui::PlotLines("Memory Usage", m_MemoryUsageHistory.data(), 
                           (i32)m_MemoryUsageHistory.size(), m_HistoryIndex, 
                           nullptr, 0.0f, FLT_MAX, ImVec2(0, 80));
        }
    }
    
    void CommandPacketDebugger::RenderPerformanceStats()
    {
        OLO_PROFILE_FUNCTION();
        
        ImGui::Text("Total Packets: %u", m_CurrentFrameStats.m_TotalPackets);
        ImGui::Text("Sorted Packets: %u", m_CurrentFrameStats.m_SortedPackets);
        ImGui::Text("Static Packets: %u", m_CurrentFrameStats.m_StaticPackets);
        ImGui::Text("Dynamic Packets: %u", m_CurrentFrameStats.m_DynamicPackets);
        ImGui::Text("State Changes: %u", m_CurrentFrameStats.m_StateChanges);
        
        ImGui::Separator();
        ImGui::Text("Sorting Time: %.3f ms", m_CurrentFrameStats.m_SortingTimeMs);
        ImGui::Text("Execution Time: %.3f ms", m_CurrentFrameStats.m_ExecutionTimeMs);
        
        // Performance graphs
        if (!m_PacketCountHistory.empty())
        {
            ImGui::PlotLines("Packet Count", m_PacketCountHistory.data(), 
                           (i32)m_PacketCountHistory.size(), m_HistoryIndex, 
                           nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
            
            ImGui::PlotLines("Sorting Time (ms)", m_SortingTimeHistory.data(), 
                           (i32)m_SortingTimeHistory.size(), m_HistoryIndex, 
                           nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
        }
    }
    
    void CommandPacketDebugger::RenderCommandPacketList(const CommandBucket* bucket)
    {
        OLO_PROFILE_FUNCTION();
        
        // Filter controls
        ImGui::Checkbox("Filter by Type", &m_FilterByType);
        if (m_FilterByType)
        {
            ImGui::SameLine();
            const char* typeNames[] = { "Draw", "Clear", "State", "Compute", "Other" };
            ImGui::Combo("Type", &m_TypeFilter, typeNames, IM_ARRAYSIZE(typeNames));
        }
        
        ImGui::Checkbox("Filter by Static", &m_FilterByStatic);
        if (m_FilterByStatic)
        {
            ImGui::SameLine();
            ImGui::Checkbox("Show Static Only", &m_StaticFilter);
        }
        
        ImGui::Separator();
        
        // Command packet table
        if (ImGui::BeginTable("CommandPackets", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Draw Key", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Static", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Debug Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Group ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();
            
            // TODO: Iterate through command packets in bucket
            // This is a placeholder - actual implementation would require access to bucket internals
            for (i32 i = 0; i < 10; ++i) // Placeholder data
            {
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                bool isSelected = (m_SelectedPacketIndex == i);
                if (ImGui::Selectable(std::to_string(i).c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_SelectedPacketIndex = i;
                }
                
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(GetColorForPacketType(nullptr), "Draw"); // Placeholder
                
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("0x%016llX", (u64)(0x1234567890ABCDEF + i)); // Placeholder
                
                ImGui::TableSetColumnIndex(3);
                ImGui::Text(i % 3 == 0 ? "Yes" : "No"); // Placeholder
                
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("Sample Command %d", i); // Placeholder
                
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%u", i / 3); // Placeholder
            }
            
            ImGui::EndTable();
        }
        
        // Selected packet details
        if (m_SelectedPacketIndex >= 0)
        {
            ImGui::Separator();
            ImGui::Text("Selected Packet Details:");
            ImGui::Indent();
            ImGui::Text("Index: %d", m_SelectedPacketIndex);
            ImGui::Text("Draw Key Breakdown:");
            ImGui::Indent();
            ImGui::Text("Layer: %u", 0); // Placeholder
            ImGui::Text("Material: %u", 0); // Placeholder
            ImGui::Text("Depth: %u", 0); // Placeholder
            ImGui::Text("Translucency: %u", 0); // Placeholder
            ImGui::Unindent();
            ImGui::Unindent();
        }
    }
    
    void CommandPacketDebugger::RenderDrawKeyAnalysis(const CommandBucket* bucket)
    {
        OLO_PROFILE_FUNCTION();
        
        ImGui::Text("Draw Key Distribution Analysis");
        ImGui::Separator();
        
        // Render histograms for different components
        RenderDrawKeyHistogram(m_DrawKeyStats.m_LayerDistribution, "Layer Distribution");
        RenderDrawKeyHistogram(m_DrawKeyStats.m_MaterialDistribution, "Material Distribution");
        RenderDrawKeyHistogram(m_DrawKeyStats.m_DepthDistribution, "Depth Distribution");
        RenderDrawKeyHistogram(m_DrawKeyStats.m_TranslucencyDistribution, "Translucency Distribution");
        
        // Sorting efficiency analysis
        ImGui::Separator();
        ImGui::Text("Sorting Efficiency:");
        ImGui::Text("- Consecutive same materials: %u%%", 85); // Placeholder
        ImGui::Text("- Consecutive same layers: %u%%", 92); // Placeholder
        ImGui::Text("- Depth sorting effectiveness: %u%%", 78); // Placeholder
    }
    
    void CommandPacketDebugger::AnalyzeDrawKeys(const CommandBucket* bucket)
    {
        OLO_PROFILE_FUNCTION();
        
        // Reset analysis data
        m_DrawKeyStats.Reset();
        
        // TODO: Implement actual draw key analysis
        // This would require access to the command bucket's internal data
        // For now, generate some placeholder data
        
        // Placeholder analysis data
        m_DrawKeyStats.m_LayerDistribution[0] = 15;
        m_DrawKeyStats.m_LayerDistribution[1] = 25;
        m_DrawKeyStats.m_LayerDistribution[2] = 8;
        
        m_DrawKeyStats.m_MaterialDistribution[0] = 12;
        m_DrawKeyStats.m_MaterialDistribution[1] = 18;
        m_DrawKeyStats.m_MaterialDistribution[2] = 8;
        m_DrawKeyStats.m_MaterialDistribution[3] = 10;
    }
    
    void CommandPacketDebugger::RenderDrawKeyHistogram(const std::unordered_map<u32, u32>& distribution, const char* label)
    {
        if (distribution.empty())
            return;
            
        ImGui::Text("%s", label);
        
        // Convert to vectors for plotting
        std::vector<f32> values;
        std::vector<std::string> labels;
        
        for (const auto& [key, count] : distribution)
        {
            labels.push_back(std::to_string(key));
            values.push_back((f32)count);
        }
        
        if (!values.empty())
        {
            ImGui::PlotHistogram("##histogram", values.data(), (i32)values.size(), 
                               0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
        }
    }
    
    std::string CommandPacketDebugger::FormatMemorySize(size_t bytes) const
    {
        const char* units[] = { "B", "KB", "MB", "GB" };
        f32 size = (f32)bytes;
        i32 unitIndex = 0;
        
        while (size >= 1024.0f && unitIndex < 3)
        {
            size /= 1024.0f;
            unitIndex++;
        }
        
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
        return ss.str();
    }
    
    ImVec4 CommandPacketDebugger::GetColorForPacketType(const CommandPacket* packet) const
    {
        // Color-code different packet types
        // For now, return placeholder colors since we need to analyze the actual packet type
        
        if (!packet)
            return ImVec4(0.8f, 0.8f, 0.8f, 1.0f); // Gray for unknown
            
        // TODO: Implement actual type detection based on command data
        return ImVec4(0.3f, 0.8f, 0.3f, 1.0f); // Green for draw commands
    }
    
    std::string CommandPacketDebugger::GetPacketTypeString(const CommandPacket* packet) const
    {
        if (!packet)
            return "Unknown";
            
        // TODO: Implement actual type detection based on command data
        return "Draw";
    }
    
    bool CommandPacketDebugger::ExportToCSV(const CommandBucket* bucket, const std::string& outputPath) const
    {
        OLO_PROFILE_FUNCTION();
        
        try
        {
            std::ofstream file(outputPath);
            if (!file.is_open())
                return false;
            
            // CSV header
            file << "Index,Type,DrawKey,Layer,Material,Depth,Translucency,Static,GroupID,DebugName\n";
            
            // TODO: Export actual packet data
            // For now, export placeholder data
            for (i32 i = 0; i < 10; ++i)
            {
                file << i << ",Draw,0x" << std::hex << (0x1234567890ABCDEF + i) << std::dec
                     << ",0,0,0,0," << (i % 3 == 0 ? "true" : "false") << "," << (i / 3) 
                     << ",Sample Command " << i << "\n";
            }
            
            file.close();
            OLO_CORE_INFO("Command packet data exported to: {0}", outputPath);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to export command packet data: {0}", e.what());
            return false;
        }
    }
}
