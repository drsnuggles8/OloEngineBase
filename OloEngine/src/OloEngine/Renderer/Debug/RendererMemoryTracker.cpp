#include "RendererMemoryTracker.h"
#include "DebugUtils.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <cmath>

namespace OloEngine
{RendererMemoryTracker& RendererMemoryTracker::GetInstance()
    {
        static RendererMemoryTracker s_Instance;
        return s_Instance;
    }
      void RendererMemoryTracker::Initialize()
    {
        OLO_PROFILE_FUNCTION();
        
        // Prevent double initialization
        if (m_IsInitialized.load())
        {
            OLO_CORE_WARN("RendererMemoryTracker: Already initialized, skipping re-initialization");
            return;
        }
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Double-check after acquiring lock
        if (m_IsInitialized.load())
        {
            OLO_CORE_WARN("RendererMemoryTracker: Already initialized (double check), skipping re-initialization");
            return;
        }
        
        // Initialize history arrays
        m_MemoryHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        m_AllocationHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        m_GPUMemoryHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        m_CPUMemoryHistory.resize(OLO_HISTORY_SIZE, 0.0f);
          // Initialize type usage tracking
        for (u32 i = 0; i < (u32)ResourceType::COUNT; ++i)
        {
            m_TypeUsage[i] = 0;
            m_TypeCounts[i] = 0;
        }
        
        m_LastUpdateTime = std::chrono::duration<f64>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
        // Set initialization flag
        m_IsInitialized.store(true);
        
        OLO_CORE_INFO("Renderer Memory Tracker initialized");
    }
	
	void RendererMemoryTracker::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        m_IsShutdown = true; // Set shutdown flag first
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        m_Allocations.clear();
        m_TypeUsage.fill(0);
        m_TypeCounts.fill(0);
        
        // Reset initialization flag
        m_IsInitialized.store(false);
        
        OLO_CORE_INFO("Renderer Memory Tracker shutdown");
    }
    
	void RendererMemoryTracker::Reset()
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
          // Clear all tracking data
        m_Allocations.clear();
        m_TypeUsage.fill(0);
        m_TypeCounts.fill(0);
        
        // Reset statistics
        m_TotalAllocatedMemory = 0;
        m_TotalDeallocatedMemory = 0;
        m_CurrentMemoryUsage = 0;
        m_PeakMemoryUsage = 0;
        m_TotalAllocations = 0;
        m_TotalDeallocations = 0;
        m_CurrentAllocations = 0;
        m_GPUMemoryUsage = 0;
        m_CPUMemoryUsage = 0;
        m_PeakGPUMemory = 0;
        m_PeakCPUMemory = 0;
        
        // Reset history arrays
        std::fill(m_MemoryHistory.begin(), m_MemoryHistory.end(), 0.0f);
        std::fill(m_AllocationHistory.begin(), m_AllocationHistory.end(), 0.0f);
        std::fill(m_GPUMemoryHistory.begin(), m_GPUMemoryHistory.end(), 0.0f);
        std::fill(m_CPUMemoryHistory.begin(), m_CPUMemoryHistory.end(), 0.0f);
        
        m_HistoryIndex = 0;
        m_LastUpdateTime = DebugUtils::GetCurrentTimeSeconds();
        
        // Reset initialization flag so Initialize() can be called again
        m_IsInitialized.store(false);
        
        OLO_CORE_INFO("Renderer Memory Tracker reset");
    }
      void RendererMemoryTracker::DebugDumpTypeUsage(const std::string& context)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        sizet total = 0;
        sizet nonZero = 0;
        for (sizet i = 0; i < static_cast<sizet>(ResourceType::COUNT); ++i)
        {
            total += m_TypeUsage[i];
            if (m_TypeUsage[i] > 0)
                nonZero++;
        }
        
        OLO_CORE_INFO("DebugDump [{}]: nonZero={}, total={}, allocations={}", 
                      context, nonZero, total, m_Allocations.size());
        
        if (nonZero > 0)
        {
            for (sizet i = 0; i < static_cast<sizet>(ResourceType::COUNT); ++i)
            {
                if (m_TypeUsage[i] > 0)
                {
                    OLO_CORE_INFO("  Type[{}] = {}", i, m_TypeUsage[i]);
                }
            }
        }
    }
      void RendererMemoryTracker::TrackAllocation(void* address, sizet size, ResourceType type,
                                               const std::string& name, bool isGPU,
                                               const char* file, u32 line)
    {
        if (!address || size == 0)
        {
            OLO_CORE_WARN("RendererMemoryTracker: Invalid allocation - address={}, size={}", address, size);
            return;
        }
            
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        AllocationInfo info;
        info.m_Address = address;
        info.m_Size = size;
        info.m_Type = type;
        info.m_Name = name;
        info.m_File = file ? file : "Unknown";
        info.m_Line = line;
        info.m_Timestamp = DebugUtils::GetCurrentTimeSeconds();
        info.m_IsGPU = isGPU;
        
        // Check for double allocation
        if (m_Allocations.find(address) != m_Allocations.end())
        {
            OLO_CORE_WARN("Double allocation detected at address {0}", address);
        }

		m_Allocations[address] = info;
        m_TypeUsage[static_cast<sizet>(type)] += size;
        
        m_TypeCounts[static_cast<sizet>(type)]++;
        m_TotalAllocations++;
        

        // Calculate total usage inline (avoid double locking)
        const sizet totalUsage = GetTotalMemoryUsageUnlocked();
        
        if (totalUsage > m_PeakMemoryUsage)
        {
            m_PeakMemoryUsage = totalUsage;
        }
    }

    void RendererMemoryTracker::TrackDeallocation(void* address)
    {
        if (!address || m_IsShutdown)
            return;
            
        // Try to acquire the mutex with a timeout to avoid deadlock during shutdown
        std::unique_lock<std::mutex> lock(m_Mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            // If we can't acquire the lock, we might be in shutdown - just return
            OLO_CORE_WARN("RendererMemoryTracker: Could not acquire lock for deallocation, possibly during shutdown");
            return;
        }
        
        // Double-check shutdown state after acquiring lock
        if (m_IsShutdown)
            return;
        
        // Check if the allocations map is still valid
        try
        { 
			auto it = m_Allocations.find(address);
            if (it != m_Allocations.end())
            {
                const AllocationInfo& info = it->second;
                m_TypeUsage[static_cast<sizet>(info.m_Type)] -= info.m_Size;
                m_TypeCounts[static_cast<sizet>(info.m_Type)]--;
                m_Allocations.erase(it);
                m_TotalDeallocations++;

            }
            else
            {
                OLO_CORE_WARN("Attempted to deallocate untracked memory at address {0}", address);
            }
        }
        catch (...)
        {
            // Catch any exceptions during shutdown
            OLO_CORE_ERROR("RendererMemoryTracker: Exception during deallocation tracking, possibly during shutdown");
        }
    }
	
	void RendererMemoryTracker::UpdateStats()
    {
        OLO_PROFILE_FUNCTION();
        f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
		if (currentTime - m_LastUpdateTime < m_RefreshInterval)
            return;

        std::lock_guard<std::mutex> lock(m_Mutex);

        sizet totalMemory = GetTotalMemoryUsageUnlocked();
        sizet gpuMemory = 0;
        sizet cpuMemory = 0;
        
        for (const auto& [address, info] : m_Allocations)
        {
            if (info.m_IsGPU)
                gpuMemory += info.m_Size;
            else
                cpuMemory += info.m_Size;
        }
        
        m_MemoryHistory[m_HistoryIndex] = (f32)totalMemory;
        m_AllocationHistory[m_HistoryIndex] = (f32)m_Allocations.size();
        m_GPUMemoryHistory[m_HistoryIndex] = (f32)gpuMemory;
        m_CPUMemoryHistory[m_HistoryIndex] = (f32)cpuMemory;
        
        m_HistoryIndex = (m_HistoryIndex + 1) % OLO_HISTORY_SIZE;
        m_LastUpdateTime = currentTime;
    }
    
    void RendererMemoryTracker::RenderUI(bool* open)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!open || *open)
        {
            ImGui::Begin("Renderer Memory Tracker", open, ImGuiWindowFlags_MenuBar);
            
            // Menu bar
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("Options"))
                {
                    ImGui::MenuItem("Show System Memory", nullptr, &m_ShowSystemMemory);
                    ImGui::MenuItem("Detailed View", nullptr, &m_ShowDetailedView);
                    ImGui::MenuItem("Enable Leak Detection", nullptr, &m_EnableLeakDetection);
                    
                    ImGui::Separator();
                    ImGui::SliderFloat("Refresh Rate", &m_RefreshInterval, 1.0f/120.0f, 1.0f, "%.3f s");
                    
                    ImGui::Separator();
                    if (ImGui::Button("Export Report"))
                    {
                        ExportReport("memory_report.txt");
                    }
                    
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            
            // Tab bar
            if (ImGui::BeginTabBar("MemoryTabs"))
            {
                if (ImGui::BeginTabItem("Overview"))
                {
                    RenderOverviewTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Detailed"))
                {
                    RenderDetailedTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Leak Detection"))
                {
                    RenderLeakDetectionTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Pool Stats"))
                {
                    RenderPoolStatsTab();
                    ImGui::EndTabItem();
                }
                
                ImGui::EndTabBar();
            }
            
            ImGui::End();
        }
    }
      void RendererMemoryTracker::RenderOverviewTab()
    {
		std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Calculate total memory usage inline (avoid double locking)
        sizet totalMemory = 0;
        for (sizet i = 0; i < static_cast<sizet>(ResourceType::COUNT); ++i)
        {
            totalMemory += m_TypeUsage[i];
        }
        
        // Debug output to see what's in the type usage array
        sizet nonZeroEntries = 0;
        for (sizet i = 0; i < static_cast<sizet>(ResourceType::COUNT); ++i)
        {
            if (m_TypeUsage[i] > 0)
                nonZeroEntries++;
        }
        
        OLO_CORE_INFO("RendererMemoryTracker: Debug - nonZeroEntries={}, m_Allocations.size()={}, totalMemory={}", 
                      nonZeroEntries, m_Allocations.size(), totalMemory);
          if (nonZeroEntries > 0)
        {
            OLO_CORE_TRACE("RendererMemoryTracker: TypeUsage array has {} entries, total = {} bytes", 
                          nonZeroEntries, totalMemory);
            for (sizet i = 0; i < static_cast<sizet>(ResourceType::COUNT); ++i)
            {
                if (m_TypeUsage[i] > 0)
                {
                    OLO_CORE_TRACE("  Type {}: {} bytes", i, m_TypeUsage[i]);
                }
            }
        }
        else
        {
            OLO_CORE_INFO("RendererMemoryTracker: TypeUsage array has NO active entries!");
        }
        
        // Summary statistics        ImGui::Text("Total Memory Usage: %s", DebugUtils::FormatMemorySize(totalMemory).c_str());
        ImGui::Text("Peak Memory Usage: %s", DebugUtils::FormatMemorySize(m_PeakMemoryUsage).c_str());
        ImGui::Text("Active Allocations: %zu", m_Allocations.size());
        ImGui::Text("Total Allocations: %zu", m_TotalAllocations);
        ImGui::Text("Total Deallocations: %zu", m_TotalDeallocations);
        
        ImGui::Separator();
          // Memory by type        ImGui::Text("Memory Usage by Type:");
        for (u32 i = 0; i < (u32)ResourceType::COUNT; ++i)
        {
            ResourceType type = (ResourceType)i;
            
            // Get usage and count inline (avoid double locking)
            sizet usage = m_TypeUsage[i];
            u32 count = m_TypeCounts[i];
            
            if (usage > 0)
            {
                ImVec4 color = GetResourceTypeColor(type);
                ImGui::TextColored(color, "%s: %s (%u allocations)", 
                                 GetResourceTypeName(type).c_str(), 
                                 DebugUtils::FormatMemorySize(usage).c_str(), count);
            }
        }
        
        ImGui::Separator();
        RenderHistoryGraphs();
    }
    
    void RendererMemoryTracker::RenderDetailedTab()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
          // Filter controls
        static i32 s_TypeFilter = -1; // -1 means show all
        static bool s_ShowGPUOnly = false;
        static bool s_ShowCPUOnly = false;
        static void* s_SelectedAllocation = nullptr; // Track selected allocation
          ImGui::Text("Filters:");
        ImGui::Combo("Resource Type", &s_TypeFilter, 
                   "All\0Vertex Buffer\0Index Buffer\0Uniform Buffer\0Texture 2D\0Texture Cubemap\0Framebuffer\0Shader\0Render Target\0Command Buffer\0Other\0");
        ImGui::Checkbox("GPU Only", &s_ShowGPUOnly);
        ImGui::SameLine();
        ImGui::Checkbox("CPU Only", &s_ShowCPUOnly);
        
        ImGui::Separator();        // Allocation table
        if (ImGui::BeginTable("Allocations", 7, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableHeadersRow();
            
            f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
            for (const auto& [address, info] : m_Allocations)
            {
                // Apply filters - fix the type filter mapping
                // s_TypeFilter: 0=All, 1=VertexBuffer, 2=IndexBuffer, etc.
                // ResourceType enum: VertexBuffer=0, IndexBuffer=1, etc.
                if (s_TypeFilter > 0 && (s_TypeFilter - 1) != (i32)info.m_Type)
                    continue;
                if (s_ShowGPUOnly && !info.m_IsGPU)
                    continue;
                if (s_ShowCPUOnly && info.m_IsGPU)
                    continue;
					
                ImGui::TableNextRow();

                // Check if this row is clicked for selection
                bool isSelected = (s_SelectedAllocation == address);
                if (isSelected)
                {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(100, 100, 150, 100));
                }
                  ImGui::TableSetColumnIndex(0);
                // Create unique ID for each selectable
                char selectableId[64];
                snprintf(selectableId, sizeof(selectableId), "##selectable_%p", address);
                if (ImGui::Selectable(selectableId, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    s_SelectedAllocation = (s_SelectedAllocation == address) ? nullptr : address;
                }
                ImGui::SameLine();
                ImGui::Text("0x%p", address);
                
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", DebugUtils::FormatMemorySize(info.m_Size).c_str());
                
                ImGui::TableSetColumnIndex(2);
                ImVec4 typeColor = GetResourceTypeColor(info.m_Type);
                ImGui::TextColored(typeColor, "%s", GetResourceTypeName(info.m_Type).c_str());
                
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", info.m_IsGPU ? "GPU" : "CPU");
                
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", info.m_Name.c_str());
                
                ImGui::TableSetColumnIndex(5);
                // Extract filename from full path
                std::string filename = info.m_File;
                sizet lastSlash = filename.find_last_of("/\\");
                if (lastSlash != std::string::npos)
                    filename = filename.substr(lastSlash + 1);
                ImGui::Text("%s:%u", filename.c_str(), info.m_Line);
                  ImGui::TableSetColumnIndex(6);
                f64 age = currentTime - info.m_Timestamp;
                ImGui::Text("%.1fs", age);
            }
            
            ImGui::EndTable();
        }
        
        // Show detailed information about selected allocation
        if (s_SelectedAllocation)
        {
            auto it = m_Allocations.find(s_SelectedAllocation);
            if (it != m_Allocations.end())
            {
                ImGui::Separator();
                ImGui::Text("Selected Allocation Details:");
                const AllocationInfo& info = it->second;
                
                ImGui::Text("Address: 0x%p", s_SelectedAllocation);
                ImGui::Text("Size: %s (%zu bytes)", DebugUtils::FormatMemorySize(info.m_Size).c_str(), info.m_Size);
                ImGui::Text("Type: %s", GetResourceTypeName(info.m_Type).c_str());
                ImGui::Text("Location: %s", info.m_IsGPU ? "GPU" : "CPU");
                ImGui::Text("Name: %s", info.m_Name.c_str());
				ImGui::Text("Source: %s:%u", info.m_File.c_str(), info.m_Line);
                
                f64 currentTime2 = DebugUtils::GetCurrentTimeSeconds();
                f64 age = currentTime2 - info.m_Timestamp;
                ImGui::Text("Age: %.2f seconds", age);
                ImGui::Text("Allocated at: %.6f", info.m_Timestamp);
                  if (ImGui::Button("Copy Address to Clipboard"))
                {
                    char addressStr[32];
                    snprintf(addressStr, sizeof(addressStr), "0x%p", s_SelectedAllocation);
                    ImGui::SetClipboardText(addressStr);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Selection"))
                {
                    s_SelectedAllocation = nullptr;
                }
            }
            else
            {
                // Selected allocation no longer exists
                s_SelectedAllocation = nullptr;
            }
        }
    }
    
    void RendererMemoryTracker::RenderLeakDetectionTab()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        ImGui::Text("Leak Detection Settings:");
        f32 threshold = static_cast<f32>(m_LeakDetectionThreshold);
        if (ImGui::SliderFloat("Detection Threshold", &threshold, 1.0f, 300.0f, "%.1f seconds"))
        {
            m_LeakDetectionThreshold = static_cast<f64>(threshold);
        }
          if (ImGui::Button("Scan for Leaks"))
        {
            m_LastLeakCheck = DebugUtils::GetCurrentTimeSeconds();
        }
          ImGui::Separator();
        
        // Detect and display potential leaks inline (avoid double locking)
        std::vector<LeakInfo> leaks;
        f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
        
        for (const auto& [address, info] : m_Allocations)
        {
            f64 age = currentTime - info.m_Timestamp;
            if (age > m_LeakDetectionThreshold)
            {
                LeakInfo leak;
                leak.m_Allocation = info;
                leak.m_AgeSeconds = age;
                leak.m_IsSuspicious = (age > m_LeakDetectionThreshold * 2.0); // Very old allocations are suspicious
                leaks.push_back(leak);
            }
        }
        
        if (leaks.empty())
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "No potential memory leaks detected!");
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Potential memory leaks detected: %zu", leaks.size());
            
            if (ImGui::BeginTable("Leaks", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Suspicious", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableHeadersRow();
                
                for (const auto& leak : leaks)
                {
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("0x%p", leak.m_Allocation.m_Address);
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", DebugUtils::FormatMemorySize(leak.m_Allocation.m_Size).c_str());
                    
                    ImGui::TableSetColumnIndex(2);
                    ImVec4 typeColor = GetResourceTypeColor(leak.m_Allocation.m_Type);
                    ImGui::TextColored(typeColor, "%s", GetResourceTypeName(leak.m_Allocation.m_Type).c_str());
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.1fs", leak.m_AgeSeconds);
                    
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%s", leak.m_Allocation.m_Name.c_str());
                    
                    ImGui::TableSetColumnIndex(5);
                    if (leak.m_IsSuspicious)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Yes");
                    }
                    else
                    {
                        ImGui::Text("No");
                    }
                }
                
                ImGui::EndTable();
            }
        }
    }
      void RendererMemoryTracker::RenderPoolStatsTab()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        ImGui::Text("Memory Pool Statistics");
        ImGui::Separator();
        
        // Calculate pool statistics based on allocation patterns
        std::map<ResourceType, std::vector<sizet>> allocationSizes;
        std::map<ResourceType, sizet> totalTypeMemory;
        
        for (const auto& [address, info] : m_Allocations)
        {
            allocationSizes[info.m_Type].push_back(info.m_Size);
            totalTypeMemory[info.m_Type] += info.m_Size;
        }
        
        // Display statistics for each resource type
        for (u32 i = 0; i < (u32)ResourceType::COUNT; ++i)
        {
            ResourceType type = (ResourceType)i;
            if (allocationSizes.find(type) == allocationSizes.end())
                continue;
                
            auto& sizes = allocationSizes[type];
            if (sizes.empty())
                continue;
                
            ImGui::Text("%s Pool:", GetResourceTypeName(type).c_str());
            ImGui::Indent();
            
            // Count and total memory
            ImGui::Text("Active Allocations: %zu", sizes.size());
            ImGui::Text("Total Memory: %s", DebugUtils::FormatMemorySize(totalTypeMemory[type]).c_str());
            
            // Calculate min, max, average
            auto minMax = std::minmax_element(sizes.begin(), sizes.end());
            sizet minSize = *minMax.first;
            sizet maxSize = *minMax.second;
            sizet avgSize = totalTypeMemory[type] / sizes.size();
              ImGui::Text("Size Range: %s - %s", DebugUtils::FormatMemorySize(minSize).c_str(), DebugUtils::FormatMemorySize(maxSize).c_str());
            ImGui::Text("Average Size: %s", DebugUtils::FormatMemorySize(avgSize).c_str());
            
            // Pool utilization (simple metric based on allocation count vs total memory)
            f32 utilization = sizes.size() > 0 ? static_cast<f32>(totalTypeMemory[type]) / (sizes.size() * maxSize) * 100.0f : 0.0f;
            ImGui::Text("Pool Utilization: %.1f%%", utilization);
            
            // Fragmentation estimate (high variance = more fragmentation)
            if (sizes.size() > 1)
            {
                f64 variance = 0.0;
                for (sizet size : sizes)
                {
                    f64 diff = static_cast<f64>(size) - static_cast<f64>(avgSize);
                    variance += diff * diff;
                }
                variance /= sizes.size();
                f64 stdDev = std::sqrt(variance);
                f32 fragmentation = static_cast<f32>(stdDev / avgSize * 100.0);
                
                ImGui::Text("Fragmentation: %.1f%% (based on size variance)", fragmentation);
            }
            
            ImGui::Unindent();
            ImGui::Separator();
        }
        
        if (allocationSizes.empty())
        {
            ImGui::Text("No active allocations to analyze");
        }
    }
    
    void RendererMemoryTracker::RenderHistoryGraphs()
    {
        // Memory usage over time
        if (!m_MemoryHistory.empty())
        {
            ImGui::Text("Memory Usage History:");
            ImGui::PlotLines("Total Memory", m_MemoryHistory.data(), 
                           (i32)m_MemoryHistory.size(), m_HistoryIndex, 
                           nullptr, 0.0f, FLT_MAX, ImVec2(0, 80));
            
            ImGui::PlotLines("GPU Memory", m_GPUMemoryHistory.data(), 
                           (i32)m_GPUMemoryHistory.size(), m_HistoryIndex, 
                           nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
            
            ImGui::PlotLines("CPU Memory", m_CPUMemoryHistory.data(), 
                           (i32)m_CPUMemoryHistory.size(), m_HistoryIndex, 
                           nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
            
            ImGui::PlotLines("Allocation Count", m_AllocationHistory.data(), 
                           (i32)m_AllocationHistory.size(), m_HistoryIndex, 
                           nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
        }
    }

    sizet RendererMemoryTracker::GetMemoryUsage(ResourceType type) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_TypeUsage[static_cast<sizet>(type)];
    }
	
	sizet RendererMemoryTracker::GetTotalMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return GetTotalMemoryUsageUnlocked();
    }
    
    sizet RendererMemoryTracker::GetTotalMemoryUsageUnlocked() const
    {
        sizet total = 0;
        for (sizet i = 0; i < static_cast<sizet>(ResourceType::COUNT); ++i)
        {
            total += m_TypeUsage[i];
        }
        return total;
    }
      u32 RendererMemoryTracker::GetAllocationCount(ResourceType type) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_TypeCounts[static_cast<sizet>(type)];
    }
    
    std::vector<RendererMemoryTracker::LeakInfo> RendererMemoryTracker::DetectLeaks() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::vector<LeakInfo> leaks;
        f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
        
        for (const auto& [address, info] : m_Allocations)
        {
            f64 age = currentTime - info.m_Timestamp;
            if (age > m_LeakDetectionThreshold)
            {
                LeakInfo leak;
                leak.m_Allocation = info;
                leak.m_AgeSeconds = age;
                leak.m_IsSuspicious = (age > m_LeakDetectionThreshold * 2.0); // Very old allocations are suspicious
                leaks.push_back(leak);
            }
        }
        
        return leaks;
    }
      std::string RendererMemoryTracker::GetResourceTypeName(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::VertexBuffer:    return "Vertex Buffer";
            case ResourceType::IndexBuffer:     return "Index Buffer";
            case ResourceType::UniformBuffer:   return "Uniform Buffer";
            case ResourceType::StorageBuffer:   return "Storage Buffer";
            case ResourceType::Texture2D:       return "Texture 2D";
            case ResourceType::TextureCubemap:  return "Texture Cubemap";
            case ResourceType::Framebuffer:     return "Framebuffer";
            case ResourceType::Shader:          return "Shader";
            case ResourceType::RenderTarget:    return "Render Target";
            case ResourceType::CommandBuffer:   return "Command Buffer";
            case ResourceType::Other:           return "Other";
            default:                            return "Unknown";
        }
    }
    
    ImVec4 RendererMemoryTracker::GetResourceTypeColor(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::VertexBuffer:    return ImVec4(0.2f, 0.8f, 0.2f, 1.0f); // Green
            case ResourceType::IndexBuffer:     return ImVec4(0.2f, 0.6f, 0.8f, 1.0f); // Blue
            case ResourceType::UniformBuffer:   return ImVec4(0.8f, 0.6f, 0.2f, 1.0f); // Orange
            case ResourceType::StorageBuffer:   return ImVec4(0.9f, 0.4f, 0.1f, 1.0f); // Dark Orange
            case ResourceType::Texture2D:       return ImVec4(0.8f, 0.2f, 0.8f, 1.0f); // Magenta
            case ResourceType::TextureCubemap:  return ImVec4(0.6f, 0.2f, 0.8f, 1.0f); // Purple
            case ResourceType::Framebuffer:     return ImVec4(0.8f, 0.2f, 0.2f, 1.0f); // Red
            case ResourceType::Shader:          return ImVec4(0.8f, 0.8f, 0.2f, 1.0f); // Yellow
            case ResourceType::RenderTarget:    return ImVec4(0.2f, 0.8f, 0.8f, 1.0f); // Cyan
            case ResourceType::CommandBuffer:   return ImVec4(0.6f, 0.8f, 0.2f, 1.0f); // Lime
            case ResourceType::Other:           return ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // Gray
            default:                            return ImVec4(0.8f, 0.8f, 0.8f, 1.0f); // Light Gray
        }
    }
    
    bool RendererMemoryTracker::ExportReport(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();
        
        try
        {
            std::ofstream file(filePath);
            if (!file.is_open())
                return false;
              std::lock_guard<std::mutex> lock(m_Mutex);
              // Calculate total memory usage inline (avoid double locking)
            sizet totalMemoryUsage = 0;
            for (sizet i = 0; i < static_cast<sizet>(ResourceType::COUNT); ++i)
            {
                totalMemoryUsage += m_TypeUsage[i];
            }
            
            file << "Renderer Memory Usage Report\n";
            file << "Generated: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";
            file << "========================================\n\n";
            
            file << "Summary:\n";            file << "Total Memory Usage: " << DebugUtils::FormatMemorySize(totalMemoryUsage) << "\n";
            file << "Peak Memory Usage: " << DebugUtils::FormatMemorySize(m_PeakMemoryUsage) << "\n";
            file << "Active Allocations: " << m_Allocations.size() << "\n";
            file << "Total Allocations: " << m_TotalAllocations << "\n";
            file << "Total Deallocations: " << m_TotalDeallocations << "\n\n";
            
            file << "Memory by Type:\n";
            for (u32 i = 0; i < (u32)ResourceType::COUNT; ++i)
            {
                ResourceType type = (ResourceType)i;
                
                // Get usage and count inline (avoid double locking)
                sizet usage = m_TypeUsage[i];
                u32 count = m_TypeCounts[i];
                
                if (usage > 0)
                {
                    file << GetResourceTypeName(type) << ": " << DebugUtils::FormatMemorySize(usage) 
                         << " (" << count << " allocations)\n";
                }
            }
            
            file << "\nDetailed Allocations:\n";            file << "Address,Size,Type,Location,Name,File,Line,Age\n";            
            f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
            for (const auto& [address, info] : m_Allocations)
            {
                f64 age = currentTime - info.m_Timestamp;
                file << std::hex << address << std::dec << ","
                     << info.m_Size << ","
                     << GetResourceTypeName(info.m_Type) << ","
                     << (info.m_IsGPU ? "GPU" : "CPU") << ","
                     << info.m_Name << ","
                     << info.m_File << ","
                     << info.m_Line << ","
                     << std::fixed << std::setprecision(1) << age << "\n";
            }
            
            file.close();
            OLO_CORE_INFO("Memory report exported to: {0}", filePath);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to export memory report: {0}", e.what());
            return false;
        }
    }
}
