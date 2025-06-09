#include "RendererMemoryTracker.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <chrono>

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
    RendererMemoryTracker& RendererMemoryTracker::GetInstance()
    {
        static RendererMemoryTracker s_Instance;
        return s_Instance;
    }
    
    void RendererMemoryTracker::Initialize()
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Initialize history arrays
        m_MemoryHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        m_AllocationHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        m_GPUMemoryHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        m_CPUMemoryHistory.resize(OLO_HISTORY_SIZE, 0.0f);
        
        // Initialize type usage tracking
        for (u32 i = 0; i < (u32)ResourceType::COUNT; ++i)
        {
            m_TypeUsage[(ResourceType)i] = 0;
            m_TypeCounts[(ResourceType)i] = 0;        }
        
        m_LastUpdateTime = std::chrono::duration<f64>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        
        OLO_CORE_INFO("Renderer Memory Tracker initialized");
    }
    
    void RendererMemoryTracker::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Check for memory leaks on shutdown
        if (!m_Allocations.empty())
        {
            OLO_CORE_WARN("Memory leaks detected on shutdown! {0} allocations not freed", m_Allocations.size());
            
            for (const auto& [address, info] : m_Allocations)
            {
                OLO_CORE_WARN("Leaked: {0} bytes ({1}) at {2}:{3} - {4}", 
                             info.m_Size, GetResourceTypeName(info.m_Type), 
                             info.m_File, info.m_Line, info.m_Name);
            }
        }
        
        m_Allocations.clear();
        m_TypeUsage.clear();
        m_TypeCounts.clear();
        
        OLO_CORE_INFO("Renderer Memory Tracker shutdown");
    }
    
    void RendererMemoryTracker::Reset()
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Clear all tracking data
        m_Allocations.clear();
        m_TypeUsage.clear();
        m_TypeCounts.clear();
        
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
        m_LastUpdateTime = GetCurrentTimeSeconds();
        
        OLO_CORE_INFO("Renderer Memory Tracker reset");
    }
    
    void RendererMemoryTracker::TrackAllocation(void* address, size_t size, ResourceType type, 
                                               const std::string& name, bool isGPU,
                                               const char* file, u32 line)
    {
        if (!address || size == 0)
            return;
            
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        AllocationInfo info;
        info.m_Address = address;
        info.m_Size = size;
        info.m_Type = type;
        info.m_Name = name;
        info.m_File = file ? file : "Unknown";
        info.m_Line = line;
        info.m_Timestamp = GetCurrentTimeSeconds();
        info.m_IsGPU = isGPU;
        
        // Check for double allocation
        if (m_Allocations.find(address) != m_Allocations.end())
        {
            OLO_CORE_WARN("Double allocation detected at address {0}", address);
        }
        
        m_Allocations[address] = info;
        m_TypeUsage[type] += size;
        m_TypeCounts[type]++;
        m_TotalAllocations++;
        
        size_t totalUsage = GetTotalMemoryUsage();
        if (totalUsage > m_PeakMemoryUsage)
        {
            m_PeakMemoryUsage = totalUsage;
        }
    }
    
    void RendererMemoryTracker::TrackDeallocation(void* address)
    {
        if (!address)
            return;
            
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        auto it = m_Allocations.find(address);
        if (it != m_Allocations.end())
        {
            const AllocationInfo& info = it->second;
            m_TypeUsage[info.m_Type] -= info.m_Size;
            m_TypeCounts[info.m_Type]--;
            m_Allocations.erase(it);
            m_TotalDeallocations++;
        }
        else
        {
            OLO_CORE_WARN("Attempted to deallocate untracked memory at address {0}", address);
        }
    }
    
    void RendererMemoryTracker::UpdateStats()
    {
        OLO_PROFILE_FUNCTION();        
        f64 currentTime = GetCurrentTimeSeconds();
        if (currentTime - m_LastUpdateTime < m_RefreshInterval)
            return;
            
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Update history
        size_t totalMemory = GetTotalMemoryUsage();
        size_t gpuMemory = 0;
        size_t cpuMemory = 0;
        
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
        
        // Summary statistics
        size_t totalMemory = GetTotalMemoryUsage();
        ImGui::Text("Total Memory Usage: %s", FormatBytes(totalMemory).c_str());
        ImGui::Text("Peak Memory Usage: %s", FormatBytes(m_PeakMemoryUsage).c_str());
        ImGui::Text("Active Allocations: %zu", m_Allocations.size());
        ImGui::Text("Total Allocations: %zu", m_TotalAllocations);
        ImGui::Text("Total Deallocations: %zu", m_TotalDeallocations);
        
        ImGui::Separator();
        
        // Memory by type
        ImGui::Text("Memory Usage by Type:");
        for (u32 i = 0; i < (u32)ResourceType::COUNT; ++i)
        {
            ResourceType type = (ResourceType)i;
            size_t usage = GetMemoryUsage(type);
            u32 count = GetAllocationCount(type);
            
            if (usage > 0)
            {
                ImVec4 color = GetResourceTypeColor(type);
                ImGui::TextColored(color, "%s: %s (%u allocations)", 
                                 GetResourceTypeName(type).c_str(), 
                                 FormatBytes(usage).c_str(), count);
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
        
        ImGui::Text("Filters:");
        ImGui::Combo("Resource Type", &s_TypeFilter, 
                   "All\0Vertex Buffer\0Index Buffer\0Uniform Buffer\0Texture 2D\0Texture Cubemap\0Framebuffer\0Shader\0Render Target\0Command Buffer\0Other\0");
        ImGui::Checkbox("GPU Only", &s_ShowGPUOnly);
        ImGui::SameLine();
        ImGui::Checkbox("CPU Only", &s_ShowCPUOnly);
        
        ImGui::Separator();
        
        // Allocation table
        if (ImGui::BeginTable("Allocations", 7, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 80.0f);            ImGui::TableHeadersRow();
            
            f64 currentTime = GetCurrentTimeSeconds();
            for (const auto& [address, info] : m_Allocations)
            {
                // Apply filters
                if (s_TypeFilter >= 0 && (i32)info.m_Type != s_TypeFilter)
                    continue;
                if (s_ShowGPUOnly && !info.m_IsGPU)
                    continue;
                if (s_ShowCPUOnly && info.m_IsGPU)
                    continue;
                    
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("0x%p", address);
                
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", FormatBytes(info.m_Size).c_str());
                
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
                size_t lastSlash = filename.find_last_of("/\\");
                if (lastSlash != std::string::npos)
                    filename = filename.substr(lastSlash + 1);
                ImGui::Text("%s:%u", filename.c_str(), info.m_Line);
                
                ImGui::TableSetColumnIndex(6);
                f64 age = currentTime - info.m_Timestamp;
                ImGui::Text("%.1fs", age);
            }
            
            ImGui::EndTable();
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
            m_LastLeakCheck = GetCurrentTimeSeconds();
        }
        
        ImGui::Separator();
        
        // Detect and display potential leaks
        auto leaks = DetectLeaks();
        
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
                    ImGui::Text("%s", FormatBytes(leak.m_Allocation.m_Size).c_str());
                    
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
        ImGui::Text("Memory Pool Statistics");
        ImGui::Separator();
        
        // TODO: Implement actual memory pool statistics
        // For now, show placeholder data
        ImGui::Text("Feature coming soon - Memory pool analysis");
        ImGui::Text("This will show:");
        ImGui::BulletText("Pool utilization");
        ImGui::BulletText("Fragmentation analysis");
        ImGui::BulletText("Allocation patterns");
        ImGui::BulletText("Pool efficiency metrics");
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
    
    size_t RendererMemoryTracker::GetMemoryUsage(ResourceType type) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_TypeUsage.find(type);
        return it != m_TypeUsage.end() ? it->second : 0;
    }
    
    size_t RendererMemoryTracker::GetTotalMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        size_t total = 0;
        for (const auto& [type, usage] : m_TypeUsage)
        {
            total += usage;
        }
        return total;
    }
    
    u32 RendererMemoryTracker::GetAllocationCount(ResourceType type) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_TypeCounts.find(type);
        return it != m_TypeCounts.end() ? it->second : 0;
    }
    
    std::vector<RendererMemoryTracker::LeakInfo> RendererMemoryTracker::DetectLeaks() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
          std::vector<LeakInfo> leaks;
        f64 currentTime = GetCurrentTimeSeconds();
        
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
    
    std::string RendererMemoryTracker::FormatBytes(size_t bytes) const
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
    
    std::string RendererMemoryTracker::GetResourceTypeName(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::VertexBuffer:    return "Vertex Buffer";
            case ResourceType::IndexBuffer:     return "Index Buffer";
            case ResourceType::UniformBuffer:   return "Uniform Buffer";
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
            
            file << "Renderer Memory Usage Report\n";
            file << "Generated: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";
            file << "========================================\n\n";
            
            file << "Summary:\n";
            file << "Total Memory Usage: " << FormatBytes(GetTotalMemoryUsage()) << "\n";
            file << "Peak Memory Usage: " << FormatBytes(m_PeakMemoryUsage) << "\n";
            file << "Active Allocations: " << m_Allocations.size() << "\n";
            file << "Total Allocations: " << m_TotalAllocations << "\n";
            file << "Total Deallocations: " << m_TotalDeallocations << "\n\n";
            
            file << "Memory by Type:\n";
            for (u32 i = 0; i < (u32)ResourceType::COUNT; ++i)
            {
                ResourceType type = (ResourceType)i;
                size_t usage = GetMemoryUsage(type);
                u32 count = GetAllocationCount(type);
                
                if (usage > 0)
                {
                    file << GetResourceTypeName(type) << ": " << FormatBytes(usage) 
                         << " (" << count << " allocations)\n";
                }
            }
            
            file << "\nDetailed Allocations:\n";            file << "Address,Size,Type,Location,Name,File,Line,Age\n";
            
            f64 currentTime = GetCurrentTimeSeconds();
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
