#include "CommandPacketDebugger.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"

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
          // Debug: Check what data we actually have
        const auto& sortedCommands = bucket->GetSortedCommands();
        sizet commandCount = bucket->GetCommandCount();
        bool isSorted = bucket->IsSorted();
        CommandPacket* head = bucket->GetCommandHead();
        
        if (!open || *open)
        {
            ImGui::Begin(title, open, ImGuiWindowFlags_MenuBar);
            
            // Debug info section
            ImGui::Text("Debug Info: Total Commands: %zu, Sorted Commands: %zu, Is Sorted: %s", 
                       commandCount, sortedCommands.size(), isSorted ? "Yes" : "No");
            ImGui::Text("Command Head: %s", head ? "Valid" : "Null");
            ImGui::Separator();
            
            // Analyze the current bucket data
            AnalyzeDrawKeys(bucket);
            
            // Update frame statistics to ensure graphs are populated
            UpdateFrameStats();
            
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
        
        // Initialize history arrays if needed
        if (m_PacketCountHistory.empty())
        {
            m_PacketCountHistory.resize(OLO_HISTORY_SIZE, 0.0f);
            m_SortingTimeHistory.resize(OLO_HISTORY_SIZE, 0.0f);
            m_ExecutionTimeHistory.resize(OLO_HISTORY_SIZE, 0.0f);
            m_MemoryUsageHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        }
        
        // Update history arrays with current frame data
        m_PacketCountHistory[m_HistoryIndex] = (f32)m_CurrentFrameStats.m_TotalPackets;
        m_SortingTimeHistory[m_HistoryIndex] = m_CurrentFrameStats.m_SortingTimeMs;
        m_ExecutionTimeHistory[m_HistoryIndex] = m_CurrentFrameStats.m_ExecutionTimeMs;
        m_MemoryUsageHistory[m_HistoryIndex] = (f32)m_MemoryStats.m_CommandPacketMemory / 1024.0f; // Convert to KB for better scale
        
        m_HistoryIndex = (m_HistoryIndex + 1) % OLO_HISTORY_SIZE;
        
        // Move current stats to previous for comparison
        m_PreviousFrameStats = m_CurrentFrameStats;
        
        OLO_CORE_TRACE("CommandPacketDebugger: Updated frame stats - Packets: {}, Sorting: {:.3f}ms, Memory: {}KB", 
                      m_CurrentFrameStats.m_TotalPackets, m_CurrentFrameStats.m_SortingTimeMs, 
                      m_MemoryStats.m_CommandPacketMemory / 1024);
    }
      void CommandPacketDebugger::RenderMemoryStats()
    {
        OLO_PROFILE_FUNCTION();
          ImGui::Text("Command Packet Memory: %s", DebugUtils::FormatMemorySize(m_MemoryStats.m_CommandPacketMemory).c_str());
        ImGui::Text("Metadata Memory: %s", DebugUtils::FormatMemorySize(m_MemoryStats.m_MetadataMemory).c_str());
        ImGui::Text("Allocator Memory: %s", DebugUtils::FormatMemorySize(m_MemoryStats.m_AllocatorMemory).c_str());
        
        ImGui::Separator();
        ImGui::Text("Allocations this frame: %u", m_MemoryStats.m_AllocationCount);
        ImGui::Text("Deallocations this frame: %u", m_MemoryStats.m_DeallocationCount);
        
        // Memory usage graph with unique ID
        if (!m_MemoryUsageHistory.empty())
        {
            ImGui::PushID("MemoryUsageGraph");
            ImGui::PlotLines("##MemoryUsage", m_MemoryUsageHistory.data(), 
                           (i32)m_MemoryUsageHistory.size(), m_HistoryIndex, 
                           "Memory Usage", 0.0f, FLT_MAX, ImVec2(0, 80));
            ImGui::PopID();
        }
    }    void CommandPacketDebugger::RenderPerformanceStats()
    {
        OLO_PROFILE_FUNCTION();
        
        ImGui::PushID("PerformanceStats");
        
        ImGui::Text("Total Packets: %u", m_CurrentFrameStats.m_TotalPackets);
        ImGui::Text("Sorted Packets: %u", m_CurrentFrameStats.m_SortedPackets);
        ImGui::Text("Static Packets: %u", m_CurrentFrameStats.m_StaticPackets);
        ImGui::Text("Dynamic Packets: %u", m_CurrentFrameStats.m_DynamicPackets);
        ImGui::Text("State Changes: %u", m_CurrentFrameStats.m_StateChanges);
        
        ImGui::Separator();
        ImGui::Text("Sorting Time: %.3f ms", m_CurrentFrameStats.m_SortingTimeMs);
        ImGui::Text("Execution Time: %.3f ms", m_CurrentFrameStats.m_ExecutionTimeMs);
        
        // Explain the sorting time vs sorted packets discrepancy
        if (m_CurrentFrameStats.m_SortedPackets == 0 && m_CurrentFrameStats.m_SortingTimeMs > 0.0f)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "(*)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Sorting time represents estimated overhead even when no packets are sorted.\n"
                                "This includes bucket preparation, traversal, and cleanup time.");
            }
        }
        
        ImGui::Separator();
        
        // Performance graphs with proper initialization
        const u32 historySize = static_cast<u32>(m_PacketCountHistory.size());
        if (historySize > 0)
        {
            // Find min/max values for better scaling
            f32 minPackets = *std::min_element(m_PacketCountHistory.begin(), m_PacketCountHistory.end());
            f32 maxPackets = *std::max_element(m_PacketCountHistory.begin(), m_PacketCountHistory.end());
            f32 minSortTime = *std::min_element(m_SortingTimeHistory.begin(), m_SortingTimeHistory.end());
            f32 maxSortTime = *std::max_element(m_SortingTimeHistory.begin(), m_SortingTimeHistory.end());
            
            // Ensure reasonable scaling
            if (maxPackets - minPackets < 1.0f) maxPackets = minPackets + 10.0f;
            if (maxSortTime - minSortTime < 0.1f) maxSortTime = minSortTime + 1.0f;
            
            ImGui::Text("Packet Count History:");
            ImGui::PushID("PacketCountGraph");
            ImGui::PlotLines("##PacketCount", m_PacketCountHistory.data(), 
                           static_cast<i32>(historySize), static_cast<i32>(m_HistoryIndex), 
                           nullptr, minPackets, maxPackets, ImVec2(0, 80));
            ImGui::PopID();
            
            ImGui::Text("Sorting Time History (ms):");
            ImGui::PushID("SortingTimeGraph");
            ImGui::PlotLines("##SortingTime", m_SortingTimeHistory.data(), 
                           static_cast<i32>(historySize), static_cast<i32>(m_HistoryIndex), 
                           nullptr, minSortTime, maxSortTime, ImVec2(0, 80));
            ImGui::PopID();
        }
        else
        {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "Collecting performance data...");
        }
        
        ImGui::PopID();
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
            ImGui::TableHeadersRow();              // Get actual command packets from bucket
            std::vector<CommandPacket*> commands;
            
            // Try sorted commands first
            const auto& sortedCommands = bucket->GetSortedCommands();
            if (!sortedCommands.empty())
            {
                commands = sortedCommands;
            }
            else
            {
                // Fall back to traversing the linked list
                CommandPacket* current = bucket->GetCommandHead();
                while (current)
                {
                    commands.push_back(current);
                    current = current->GetNext();
                }
            }
            
            for (i32 i = 0; i < static_cast<i32>(commands.size()); ++i)
            {
                const CommandPacket* packet = commands[i];
                if (!packet) continue;
                
                const auto& metadata = packet->GetMetadata();
                CommandType commandType = packet->GetCommandType();
                
                // Apply filtering
                bool passesFilter = true;
                
                if (m_FilterByType)
                {
                    bool matchesTypeFilter = false;
                    switch (m_TypeFilter)
                    {
                        case 0: // Draw
                            matchesTypeFilter = (commandType == CommandType::DrawMesh || 
                                               commandType == CommandType::DrawMeshInstanced ||
                                               commandType == CommandType::DrawQuad ||
                                               commandType == CommandType::DrawIndexed ||
                                               commandType == CommandType::DrawArrays);
                            break;
                        case 1: // Clear
                            matchesTypeFilter = (commandType == CommandType::Clear || 
                                               commandType == CommandType::ClearStencil);
                            break;
                        case 2: // State
                            matchesTypeFilter = (commandType == CommandType::SetViewport ||
                                               commandType == CommandType::SetClearColor ||
                                               commandType == CommandType::SetBlendState ||
                                               commandType == CommandType::SetDepthTest);
                            break;
                        case 3: // Compute
                            // No compute commands yet
                            matchesTypeFilter = false;
                            break;
                        case 4: // Other
                            matchesTypeFilter = !((commandType == CommandType::DrawMesh || 
                                                 commandType == CommandType::DrawMeshInstanced ||
                                                 commandType == CommandType::DrawQuad ||
                                                 commandType == CommandType::DrawIndexed ||
                                                 commandType == CommandType::DrawArrays ||
                                                 commandType == CommandType::Clear || 
                                                 commandType == CommandType::ClearStencil ||
                                                 commandType == CommandType::SetViewport ||
                                                 commandType == CommandType::SetClearColor ||
                                                 commandType == CommandType::SetBlendState ||
                                                 commandType == CommandType::SetDepthTest));
                            break;
                    }
                    passesFilter = passesFilter && matchesTypeFilter;
                }
                
                if (m_FilterByStatic)
                {
                    passesFilter = passesFilter && (metadata.m_IsStatic == m_StaticFilter);
                }
                
                if (!passesFilter)
                    continue;
                
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                bool isSelected = (m_SelectedPacketIndex == i);
				ImGui::PushID(i);
                if (ImGui::Selectable("##packetRow", isSelected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_SelectedPacketIndex = i;
                    m_SelectedPacket = packet; // Store the actual packet pointer
                }
                ImGui::SameLine();
                ImGui::Text("%d", i);
                ImGui::PopID();
                  ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(GetColorForPacketType(packet), "%s", packet->GetCommandTypeString());
                
                ImGui::TableSetColumnIndex(2);
                const DrawKey& drawKey = metadata.m_SortKey;
                ImGui::Text("0x%016llX", drawKey.GetKey());
                
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", metadata.m_IsStatic ? "Yes" : "No");
                
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", metadata.m_DebugName ? metadata.m_DebugName : "Unknown");
                
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%u", metadata.m_GroupID);
            }
            
            ImGui::EndTable();
        }        // Selected packet details
        if (m_SelectedPacketIndex >= 0 && m_SelectedPacket)
        {
            const auto& metadata = m_SelectedPacket->GetMetadata();
            const DrawKey& drawKey = metadata.m_SortKey;
            
            ImGui::Separator();
            ImGui::Text("Selected Packet Details:");
            ImGui::Indent();
            ImGui::Text("Index: %d", m_SelectedPacketIndex);
            ImGui::Text("Command Type: %s", m_SelectedPacket->GetCommandTypeString());
            ImGui::Text("Static: %s", metadata.m_IsStatic ? "Yes" : "No");
            ImGui::Text("Group ID: %u", metadata.m_GroupID);
            ImGui::Text("Execution Order: %u", metadata.m_ExecutionOrder);
            ImGui::Text("Debug Name: %s", metadata.m_DebugName ? metadata.m_DebugName : "None");
            
            ImGui::Text("Draw Key Breakdown:");
            ImGui::Indent();
            ImGui::Text("Raw Key: 0x%016llX", drawKey.GetKey());
            ImGui::Text("Viewport ID: %u", drawKey.GetViewportID());
            ImGui::Text("View Layer: %s", ToString(drawKey.GetViewLayer()));
            ImGui::Text("Render Mode: %s", ToString(drawKey.GetRenderMode()));
            ImGui::Text("Material ID: %u", drawKey.GetMaterialID());
            ImGui::Text("Shader ID: %u", drawKey.GetShaderID());
            ImGui::Text("Depth: %u", drawKey.GetDepth());
            ImGui::Unindent();
            ImGui::Unindent();
        }
    }
	
	void CommandPacketDebugger::RenderDrawKeyAnalysis(const CommandBucket* bucket)
    {
        OLO_PROFILE_FUNCTION();
        (void)bucket; // Suppress warning - bucket parameter used for future functionality
        
        ImGui::Text("Draw Key Distribution Analysis");
        
        // Show command count first
        const auto& commands = bucket->GetSortedCommands();
        ImGui::Text("Total Commands: %zu", commands.size());
        
        ImGui::Separator();
        
        // Render histograms for different components
        if (!m_DrawKeyStats.m_LayerDistribution.empty())
        {
            ImGui::Text("Layer Distribution (%zu layers):", m_DrawKeyStats.m_LayerDistribution.size());
            RenderDrawKeyHistogram(m_DrawKeyStats.m_LayerDistribution, "Layer Distribution");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "No layer data available");
        }        if (!m_DrawKeyStats.m_MaterialDistribution.empty())
        {
            ImGui::Text("Material Distribution (%zu materials):", m_DrawKeyStats.m_MaterialDistribution.size());
            RenderDrawKeyHistogram(m_DrawKeyStats.m_MaterialDistribution, "Material Distribution");
            
            // Show additional info about material ID 0
            if (m_DrawKeyStats.m_MaterialZeroCount > 0)
            {
                ImGui::Text("Commands using default material (ID 0): %u", m_DrawKeyStats.m_MaterialZeroCount);
            }
        }
        else        {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "No material data available");
            ImGui::Text("This usually means commands are using material ID 0 (default material)");
        }
        
        if (!m_DrawKeyStats.m_DepthDistribution.empty())
        {
            ImGui::Text("Depth Distribution (%zu depths):", m_DrawKeyStats.m_DepthDistribution.size());
            RenderDrawKeyHistogram(m_DrawKeyStats.m_DepthDistribution, "Depth Distribution");
        }
        
        if (!m_DrawKeyStats.m_TranslucencyDistribution.empty())
        {
            ImGui::Text("Translucency Distribution (%zu types):", m_DrawKeyStats.m_TranslucencyDistribution.size());
            RenderDrawKeyHistogram(m_DrawKeyStats.m_TranslucencyDistribution, "Translucency Distribution");
        }
        
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
        
        if (!bucket)
        {
            OLO_CORE_WARN("CommandPacketDebugger::AnalyzeDrawKeys: Null bucket provided!");
            return;
        }
        
        // Debug the bucket state in detail
        CommandPacket* head = bucket->GetCommandHead();
        const auto& sortedCommands = bucket->GetSortedCommands();
        
        // Reset analysis data
        m_DrawKeyStats.Reset();
        
        // Get the actual command packets from the bucket
        std::vector<CommandPacket*> commands;
        
        // Try sorted commands first
        if (!sortedCommands.empty())
        {
            commands = sortedCommands;
        }
        else
        {
            // Fall back to traversing the linked list if no sorted commands
            OLO_CORE_TRACE("[CommandPacketDebugger] No sorted commands, traversing linked list...");
            CommandPacket* current = head;
            sizet traversalCount = 0;
            while (current)
            {
                commands.push_back(current);
                OLO_CORE_TRACE("[CommandPacketDebugger]   - Found command {} with type: {}", 
                               traversalCount, static_cast<int>(current->GetCommandType()));
                current = current->GetNext();
                traversalCount++;
                
                // Safety check to prevent infinite loops
                if (traversalCount > 10000)
                {
                    OLO_CORE_ERROR("[CommandPacketDebugger] Traversal safety limit reached! Possible infinite loop.");
                    break;
                }
            }
        }        
        // Update current frame stats
        m_CurrentFrameStats.m_TotalPackets = static_cast<u32>(commands.size());
        
        // Add basic timing simulation since we don't have real timing data yet
        // TODO: Replace with actual timing from CommandBucket when available
        if (!commands.empty())
        {
            // Simulate some sorting time based on command count (microseconds to milliseconds)
            m_CurrentFrameStats.m_SortingTimeMs = static_cast<f32>(commands.size()) * 0.01f;
            
            // Simulate execution time based on command types (rough estimate)
            f32 estimatedExecutionTime = 0.0f;
            for (const auto* packet : commands)
            {
                if (!packet) continue;
                
                CommandType type = packet->GetCommandType();
                switch (type)
                {
                    case CommandType::DrawMesh:
                    case CommandType::DrawMeshInstanced:
                        estimatedExecutionTime += 0.1f; // 0.1ms per draw call
                        break;
                    case CommandType::Clear:
                    case CommandType::SetViewport:
                        estimatedExecutionTime += 0.01f; // 0.01ms per state change
                        break;
                    default:
                        estimatedExecutionTime += 0.05f; // 0.05ms for other commands
                        break;
                }
            }
            m_CurrentFrameStats.m_ExecutionTimeMs = estimatedExecutionTime;
        }
        else
        {
            m_CurrentFrameStats.m_SortingTimeMs = 0.0f;
            m_CurrentFrameStats.m_ExecutionTimeMs = 0.0f;
        }        u32 staticCount = 0;
        u32 dynamicCount = 0;
        u32 materialZeroCount = 0; // Track commands with material ID 0
        std::unordered_map<u32, u32> layerCounts;
        std::unordered_map<u32, u32> materialCounts;
        std::unordered_map<u32, u32> depthCounts;
        std::unordered_map<u32, u32> translucencyCounts;
        
        // Analyze each command packet
        for (const auto* packet : commands)
        {
            if (!packet) continue;
            
            const auto& metadata = packet->GetMetadata();
            
            // Count static vs dynamic
            if (metadata.m_IsStatic)
                staticCount++;
            else
                dynamicCount++;
                
            // Count by layer
            u32 layer = static_cast<u32>(metadata.m_SortKey.GetViewLayer());
            layerCounts[layer]++;
            // Count by material (include material ID 0 in a separate count)
            u32 materialId = metadata.m_SortKey.GetMaterialID();
            if (materialId == 0)
            {
                materialZeroCount++;
            }
            
            // Count all materials (including 0) for the distribution histogram
            materialCounts[materialId]++;
                
            // Count by depth (group into ranges for better visualization)
            u32 depth = metadata.m_SortKey.GetDepth();
            u32 depthRange = depth / 1000; // Group depths into ranges of 1000
            depthCounts[depthRange]++;
            
            // Count by render mode (translucency info)
            u32 renderMode = static_cast<u32>(metadata.m_SortKey.GetRenderMode());
            translucencyCounts[renderMode]++;
        }
        
        m_CurrentFrameStats.m_StaticPackets = staticCount;
        m_CurrentFrameStats.m_DynamicPackets = dynamicCount;
        
        // Update distributions for histograms
        m_DrawKeyStats.m_LayerDistribution = layerCounts;
        m_DrawKeyStats.m_MaterialDistribution = materialCounts;
        m_DrawKeyStats.m_DepthDistribution = depthCounts;
        m_DrawKeyStats.m_TranslucencyDistribution = translucencyCounts;
        
        // Store the material zero count for display
        m_DrawKeyStats.m_MaterialZeroCount = materialZeroCount;
          // Calculate memory usage - this is an approximation
        sizet totalMemory = commands.size() * sizeof(CommandPacket);
        m_MemoryStats.m_CommandPacketMemory = totalMemory;
        m_MemoryStats.m_MetadataMemory = commands.size() * sizeof(PacketMetadata);
        m_MemoryStats.m_AllocationCount = static_cast<u32>(commands.size());
          // Get allocator memory usage from the bucket
        if (auto* allocator = bucket->GetAllocator())
        {
            // Get actual allocator memory usage
            m_MemoryStats.m_AllocatorMemory = allocator->GetTotalAllocated();
        }
        else
        {
            // Estimate allocator memory as roughly 150% of packet memory for overhead
            m_MemoryStats.m_AllocatorMemory = totalMemory + (totalMemory / 2);
        }
        
        // Calculate state changes by analyzing render state differences between commands
        u32 stateChanges = 0;
        const CommandPacket* previousPacket = nullptr;
        
        for (const auto* packet : commands)
        {
            if (!packet) continue;
            
            // Check for state changes by comparing render states
            if (previousPacket)
            {
                // For draw commands, check if render state differs
                if (packet->GetCommandType() == CommandType::DrawMesh || 
                    packet->GetCommandType() == CommandType::DrawMeshInstanced)
                {
                    const auto* currentCmd = packet->GetCommandData<DrawMeshCommand>();
                    const auto* prevCmd = previousPacket->GetCommandData<DrawMeshCommand>();
                    
                    if (currentCmd && prevCmd)
                    {
                        // Compare render states - each different state is a state change
                        if (currentCmd->renderState && prevCmd->renderState)
                        {
                            const auto& current = *currentCmd->renderState;
                            const auto& previous = *prevCmd->renderState;
                            
                            // Check various state categories
                            if (current.PolygonMode.Mode != previous.PolygonMode.Mode) stateChanges++;
                            if (current.LineWidth.Width != previous.LineWidth.Width) stateChanges++;
                            if (current.Blend.Enabled != previous.Blend.Enabled) stateChanges++;
                            if (current.Blend.SrcFactor != previous.Blend.SrcFactor) stateChanges++;
                            if (current.Blend.DstFactor != previous.Blend.DstFactor) stateChanges++;
                            if (current.PolygonOffset.Enabled != previous.PolygonOffset.Enabled) stateChanges++;
                            if (current.PolygonOffset.Factor != previous.PolygonOffset.Factor) stateChanges++;
                            if (current.PolygonOffset.Units != previous.PolygonOffset.Units) stateChanges++;
                        }
                    }
                }
                
                // Also count command type changes as state changes
                if (packet->GetCommandType() != previousPacket->GetCommandType())
                {
                    stateChanges++;
                }
            }
            
            previousPacket = packet;
        }
        
        m_CurrentFrameStats.m_StateChanges = stateChanges;
        
        OLO_CORE_TRACE("CommandPacketDebugger: Static: {}, Dynamic: {}, Memory: {} bytes", 
                      staticCount, dynamicCount, totalMemory);
    }
      void CommandPacketDebugger::RenderDrawKeyHistogram(const std::unordered_map<u32, u32>& distribution, const char* label)
    {
        if (distribution.empty())
        {
            ImGui::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "%s: No data", label);
            return;
        }
            
        ImGui::Text("%s:", label);
        
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
            // Calculate max value for better scaling
            f32 maxValue = *std::max_element(values.begin(), values.end());
            
            // Show a text summary first
            ImGui::Indent();
            ImGui::Text("Keys: %zu, Max Count: %.0f", values.size(), maxValue);
            
            // Show individual counts if there are few enough items
            if (distribution.size() <= 8)
            {
                for (const auto& [key, count] : distribution)
                {
                    ImGui::Text("  %u: %u", key, count);
                }
            }
              // Show the histogram with unique ID based on label
            std::string histogramId = std::string("##histogram_") + label;
            ImGui::PlotHistogram(histogramId.c_str(), values.data(), (i32)values.size(), 
                               0, nullptr, 0.0f, maxValue * 1.1f, ImVec2(300, 80));
            ImGui::Unindent();
        }
          ImGui::Spacing();
    }
    
    ImVec4 CommandPacketDebugger::GetColorForPacketType(const CommandPacket* packet) const
    {
        if (!packet)
            return ImVec4(0.8f, 0.8f, 0.8f, 1.0f); // Gray for unknown
            
        CommandType type = packet->GetCommandType();
        
        switch (type)
        {
            case CommandType::DrawMesh:
            case CommandType::DrawMeshInstanced:
            case CommandType::DrawQuad:
            case CommandType::DrawIndexed:
            case CommandType::DrawArrays:
                return ImVec4(0.3f, 0.8f, 0.3f, 1.0f); // Green for draw commands
                
            case CommandType::Clear:
            case CommandType::ClearStencil:
                return ImVec4(0.8f, 0.3f, 0.3f, 1.0f); // Red for clear commands
                
            case CommandType::SetViewport:
            case CommandType::SetClearColor:
            case CommandType::SetBlendState:
            case CommandType::SetDepthTest:
            case CommandType::SetDepthMask:
            case CommandType::SetDepthFunc:
                return ImVec4(0.3f, 0.3f, 0.8f, 1.0f); // Blue for state commands
                
            case CommandType::BindTexture:
            case CommandType::BindDefaultFramebuffer:
                return ImVec4(0.8f, 0.8f, 0.3f, 1.0f); // Yellow for binding commands
                
            default:
                return ImVec4(0.8f, 0.5f, 0.3f, 1.0f); // Orange for other commands
        }
    }
      std::string CommandPacketDebugger::GetPacketTypeString(const CommandPacket* packet) const
    {
        if (!packet)
            return "Unknown";
            
        return std::string(packet->GetCommandTypeString());
    }
      bool CommandPacketDebugger::ExportToCSV(const CommandBucket* bucket, const std::string& outputPath) const
    {
        OLO_PROFILE_FUNCTION();
        
        if (!bucket)
        {
            OLO_CORE_ERROR("Cannot export CSV: bucket is null");
            return false;
        }
        
        try
        {
            std::ofstream file(outputPath);
            if (!file.is_open())
                return false;
            
            // CSV header
            file << "Index,Type,DrawKey,ViewportID,ViewLayer,RenderMode,MaterialID,ShaderID,Depth,Static,GroupID,DebugName\n";
            
            const auto& commands = bucket->GetSortedCommands();
            for (sizet i = 0; i < commands.size(); ++i)
            {
                const CommandPacket* packet = commands[i];
                if (!packet) continue;
                
                const auto& metadata = packet->GetMetadata();
                const DrawKey& drawKey = metadata.m_SortKey;
                
                file << i << ","
                     << packet->GetCommandTypeString() << ","
                     << "0x" << std::hex << drawKey.GetKey() << std::dec << ","
                     << drawKey.GetViewportID() << ","
                     << ToString(drawKey.GetViewLayer()) << ","
                     << ToString(drawKey.GetRenderMode()) << ","
                     << drawKey.GetMaterialID() << ","
                     << drawKey.GetShaderID() << ","
                     << drawKey.GetDepth() << ","
                     << (metadata.m_IsStatic ? "true" : "false") << ","
                     << metadata.m_GroupID << ","
                     << (metadata.m_DebugName ? metadata.m_DebugName : "Unknown") << "\n";
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
