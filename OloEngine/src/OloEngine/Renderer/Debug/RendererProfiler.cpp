#include "RendererProfiler.h"
#include "DebugUtils.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace OloEngine
{
    RendererProfiler& RendererProfiler::GetInstance()
    {
        static RendererProfiler s_Instance;
        return s_Instance;
    }
    
    void RendererProfiler::Initialize()
    {
        OLO_PROFILE_FUNCTION();
          // Initialize performance counters
        for (u32 i = 0; i < (u32)MetricType::COUNT; ++i)
        {
            m_Counters[(MetricType)i] = PerformanceCounter{};
            m_Counters[(MetricType)i].Reset();  // sets m_Min = DBL_MAX etc.
        }
        
        // Initialize frame history
        m_FrameHistory.resize(OLO_FRAME_HISTORY_SIZE);
        
        m_LastUpdateTime = DebugUtils::GetCurrentTimeSeconds();
        m_LastFrameTime = std::chrono::high_resolution_clock::now();
        
        OLO_CORE_INFO("Renderer Profiler initialized");
    }
    
    void RendererProfiler::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        m_Counters.clear();
        m_CustomTimings.clear();
        m_FrameHistory.clear();
        
        OLO_CORE_INFO("Renderer Profiler shutdown");
    }
      void RendererProfiler::Reset()
    {
        OLO_PROFILE_FUNCTION();
        
        for (auto& [type, counter] : m_Counters)
        {
            counter.Reset();
        }
        m_CustomTimings.clear();
        m_FrameHistory.clear();
        m_FrameHistory.resize(OLO_FRAME_HISTORY_SIZE);
        m_HistoryIndex = 0;
        
        // Reset current frame data
        m_CurrentFrame = {};
        
        // Reset timing data
        m_FrameStartTime = std::chrono::high_resolution_clock::now();
        m_LastFrameTime = m_FrameStartTime;
        
        OLO_CORE_INFO("Renderer Profiler reset");
    }
      void RendererProfiler::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();
        m_FrameStartTime = std::chrono::high_resolution_clock::now();
        m_FrameNumber++;
        
        // Calculate frame time from previous frame
        auto frameTime = std::chrono::duration<f64, std::milli>(m_FrameStartTime - m_LastFrameTime).count();
        m_CurrentFrame.m_FrameTime = frameTime;
        m_LastFrameTime = m_FrameStartTime;
        
        // Reset frame counters
        m_CurrentFrame.m_DrawCalls = 0;
        m_CurrentFrame.m_StateChanges = 0;
        m_CurrentFrame.m_ShaderBinds = 0;
        m_CurrentFrame.m_TextureBinds = 0;
        m_CurrentFrame.m_BufferBinds = 0;
        m_CurrentFrame.m_VerticesRendered = 0;
        m_CurrentFrame.m_TrianglesRendered = 0;
        m_CurrentFrame.m_CommandPackets = 0;
        m_CurrentFrame.m_SortingTime = 0.0;
        m_CurrentFrame.m_CullingTime = 0.0;
    }
    
    void RendererProfiler::EndFrame()
    {
        OLO_PROFILE_FUNCTION();
        
        // Calculate CPU frame time
        auto frameEndTime = std::chrono::high_resolution_clock::now();
        m_CurrentFrame.m_CPUTime = std::chrono::duration<f64, std::milli>(frameEndTime - m_FrameStartTime).count();
        
        // Store frame data in history
        m_FrameHistory[m_HistoryIndex] = m_CurrentFrame;
        m_HistoryIndex = (m_HistoryIndex + 1) % OLO_FRAME_HISTORY_SIZE;
        
        // Update performance counters
        m_Counters[MetricType::FrameTime].AddSample(m_CurrentFrame.m_FrameTime);
        m_Counters[MetricType::CPUTime].AddSample(m_CurrentFrame.m_CPUTime);
        m_Counters[MetricType::GPUTime].AddSample(m_CurrentFrame.m_GPUTime);
        m_Counters[MetricType::DrawCalls].AddSample(m_CurrentFrame.m_DrawCalls);
        m_Counters[MetricType::StateChanges].AddSample(m_CurrentFrame.m_StateChanges);
        m_Counters[MetricType::ShaderBinds].AddSample(m_CurrentFrame.m_ShaderBinds);
        m_Counters[MetricType::TextureBinds].AddSample(m_CurrentFrame.m_TextureBinds);
        m_Counters[MetricType::BufferBinds].AddSample(m_CurrentFrame.m_BufferBinds);
        m_Counters[MetricType::VerticesRendered].AddSample(m_CurrentFrame.m_VerticesRendered);
        m_Counters[MetricType::TrianglesRendered].AddSample(m_CurrentFrame.m_TrianglesRendered);
        m_Counters[MetricType::CommandPackets].AddSample(m_CurrentFrame.m_CommandPackets);
        m_Counters[MetricType::SortingTime].AddSample(m_CurrentFrame.m_SortingTime);
        m_Counters[MetricType::CullingTime].AddSample(m_CurrentFrame.m_CullingTime);
        
        // Move current to previous
        m_PreviousFrame = m_CurrentFrame;
    }
    
    void RendererProfiler::AddTimingSample(const std::string& name, f64 timeMs, MetricType type)
    {
        m_CustomTimings[name].AddSample(timeMs);
        
        // Also add to the appropriate metric type
        if (type == MetricType::CPUTime)
            m_CurrentFrame.m_CPUTime += timeMs;
        else if (type == MetricType::GPUTime)
            m_CurrentFrame.m_GPUTime += timeMs;
        else if (type == MetricType::SortingTime)
            m_CurrentFrame.m_SortingTime += timeMs;
        else if (type == MetricType::CullingTime)
            m_CurrentFrame.m_CullingTime += timeMs;
    }
    
    void RendererProfiler::IncrementCounter(MetricType type, u32 value)
    {
        switch (type)
        {
            case MetricType::DrawCalls:
                m_CurrentFrame.m_DrawCalls += value;
                break;
            case MetricType::StateChanges:
                m_CurrentFrame.m_StateChanges += value;
                break;
            case MetricType::ShaderBinds:
                m_CurrentFrame.m_ShaderBinds += value;
                break;
            case MetricType::TextureBinds:
                m_CurrentFrame.m_TextureBinds += value;
                break;
            case MetricType::BufferBinds:
                m_CurrentFrame.m_BufferBinds += value;
                break;
            case MetricType::VerticesRendered:
                m_CurrentFrame.m_VerticesRendered += value;
                break;
            case MetricType::TrianglesRendered:
                m_CurrentFrame.m_TrianglesRendered += value;
                break;
            case MetricType::CommandPackets:
                m_CurrentFrame.m_CommandPackets += value;
                break;
            default:
                break;
        }
    }
    
    void RendererProfiler::SetValue(MetricType type, f64 value)
    {
        switch (type)
        {
            case MetricType::GPUTime:
                m_CurrentFrame.m_GPUTime = value;
                break;
            default:
                break;
        }
    }
    
    void RendererProfiler::RenderUI(bool* open)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!open || *open)
        {
            ImGui::Begin("Renderer Performance Profiler", open, ImGuiWindowFlags_MenuBar);
            
            // Menu bar
            if (ImGui::BeginMenuBar())
            {
                if (ImGui::BeginMenu("Options"))
                {
                    ImGui::MenuItem("Enable GPU Timing", nullptr, &m_EnableGPUTiming);
                    ImGui::MenuItem("Show Advanced Metrics", nullptr, &m_ShowAdvancedMetrics);
                    ImGui::MenuItem("Auto Analyze Bottlenecks", nullptr, &m_AutoAnalyzeBottlenecks);
                    ImGui::MenuItem("Pause Updates", nullptr, &m_PauseUpdates);
                    
                    ImGui::Separator();
                    ImGui::SliderFloat("Target Frame Rate", &m_TargetFrameRate, 30.0f, 144.0f, "%.0f FPS");
                    
                    ImGui::Separator();
                    if (ImGui::Button("Export CSV"))
                    {
                        ExportToCSV("renderer_performance.csv");
                    }
                    
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            
            // Tab bar
            if (ImGui::BeginTabBar("ProfilerTabs"))
            {
                if (ImGui::BeginTabItem("Overview"))
                {
                    RenderOverviewTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Detailed Timing"))
                {
                    RenderDetailedTimingTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Bottleneck Analysis"))
                {
                    RenderBottleneckAnalysisTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Counters"))
                {
                    RenderCountersTab();
                    ImGui::EndTabItem();
                }
                  if (ImGui::BeginTabItem("History"))
                {
                    RenderHistoryTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Frame Capture"))
                {
                    RenderFrameCaptureTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Frame Compare"))
                {
                    RenderFrameComparisonTab();
                    ImGui::EndTabItem();
                }
                
                ImGui::EndTabBar();
            }
            
            ImGui::End();
        }
    }
    
    void RendererProfiler::RenderOverviewTab()
    {
        // Frame rate and timing overview
        f32 currentFPS = CalculateFrameRate();
        f32 avgFrameTime = CalculateAverageFrameTime();
        
        ImGui::Text("Frame Rate: %.1f FPS", currentFPS);
        ImGui::SameLine();
        if (currentFPS < m_TargetFrameRate * 0.95f)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "(Below Target)");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "(On Target)");
        }
        
        ImGui::Text("Frame Time: %.2f ms (avg: %.2f ms)", m_CurrentFrame.m_FrameTime, avgFrameTime);
        ImGui::Text("CPU Time: %.2f ms", m_CurrentFrame.m_CPUTime);
        
        if (m_EnableGPUTiming)
        {
            ImGui::Text("GPU Time: %.2f ms", m_CurrentFrame.m_GPUTime);
            
            // CPU vs GPU utilization
            f32 cpuPercent = (f32)(m_CurrentFrame.m_CPUTime / m_CurrentFrame.m_FrameTime * 100.0);
            f32 gpuPercent = (f32)(m_CurrentFrame.m_GPUTime / m_CurrentFrame.m_FrameTime * 100.0);
            
            ImGui::Separator();
            ImGui::Text("CPU Utilization: %.1f%%", cpuPercent);
            ImGui::ProgressBar(cpuPercent / 100.0f, ImVec2(0.0f, 0.0f));
            
            ImGui::Text("GPU Utilization: %.1f%%", gpuPercent);
            ImGui::ProgressBar(gpuPercent / 100.0f, ImVec2(0.0f, 0.0f));
        }
        
        ImGui::Separator();
        
        // Key performance metrics
        ImGui::Text("Draw Calls: %u", m_CurrentFrame.m_DrawCalls);
        ImGui::Text("State Changes: %u", m_CurrentFrame.m_StateChanges);
        ImGui::Text("Vertices: %u", m_CurrentFrame.m_VerticesRendered);
        ImGui::Text("Triangles: %u", m_CurrentFrame.m_TrianglesRendered);
        
        if (m_ShowAdvancedMetrics)
        {
            ImGui::Separator();
            ImGui::Text("Command Packets: %u", m_CurrentFrame.m_CommandPackets);
            ImGui::Text("Sorting Time: %.2f ms", m_CurrentFrame.m_SortingTime);
            ImGui::Text("Culling Time: %.2f ms", m_CurrentFrame.m_CullingTime);
            ImGui::Text("Shader Binds: %u", m_CurrentFrame.m_ShaderBinds);
            ImGui::Text("Texture Binds: %u", m_CurrentFrame.m_TextureBinds);
            ImGui::Text("Buffer Binds: %u", m_CurrentFrame.m_BufferBinds);
        }
        
        // Quick performance indicators
        ImGui::Separator();
        ImGui::Text("Performance Indicators:");
        
        // High draw call count warning
        if (m_CurrentFrame.m_DrawCalls > 1000)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ High draw call count");
        }
        
        // High state change warning
        if (m_CurrentFrame.m_StateChanges > 500)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ High state change count");
        }
        
        // Low triangle to draw call ratio
        if (m_CurrentFrame.m_DrawCalls > 0)
        {
            f32 trianglesPerDraw = (f32)m_CurrentFrame.m_TrianglesRendered / (f32)m_CurrentFrame.m_DrawCalls;
            if (trianglesPerDraw < 100.0f)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "⚠ Low triangles per draw call (%.1f)", trianglesPerDraw);
            }
        }
    }
    
    void RendererProfiler::RenderDetailedTimingTab()
    {
        ImGui::Text("Custom Timing Samples:");
        
        if (ImGui::BeginTable("TimingTable", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Average", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();
            
            for (const auto& [name, counter] : m_CustomTimings)
            {
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%s", name.c_str());
                
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.2f ms", counter.m_Value);
                
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f ms", counter.m_Average);
                
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f ms", counter.m_Min == DBL_MAX ? 0.0 : counter.m_Min);
                
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.2f ms", counter.m_Max);
            }
            
            ImGui::EndTable();
        }
    }
    
    void RendererProfiler::RenderBottleneckAnalysisTab()
    {
        if (m_AutoAnalyzeBottlenecks)
        {
            BottleneckInfo bottleneck = AnalyzeBottlenecks();
            
            ImGui::Text("Bottleneck Analysis:");
            ImGui::Separator();
            
            // Display bottleneck type with color coding
            ImVec4 color;
            switch (bottleneck.m_Type)
            {
                case BottleneckInfo::CPU_Bound:     color = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;
                case BottleneckInfo::GPU_Bound:     color = ImVec4(1.0f, 0.0f, 0.5f, 1.0f); break;
                case BottleneckInfo::Memory_Bound:  color = ImVec4(0.5f, 0.0f, 1.0f, 1.0f); break;
                case BottleneckInfo::IO_Bound:      color = ImVec4(0.0f, 0.5f, 1.0f, 1.0f); break;
                case BottleneckInfo::Balanced:      color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); break;
            }
            
            const char* typeNames[] = { "CPU Bound", "GPU Bound", "Memory Bound", "I/O Bound", "Balanced" };
            ImGui::TextColored(color, "Primary Bottleneck: %s", typeNames[(i32)bottleneck.m_Type]);
            ImGui::Text("Confidence: %.1f%%", bottleneck.m_Confidence * 100.0f);
            
            ImGui::Separator();
            ImGui::Text("Description:");
            ImGui::TextWrapped("%s", bottleneck.m_Description.c_str());
            
            if (!bottleneck.m_Recommendations.empty())
            {
                ImGui::Separator();
                ImGui::Text("Recommendations:");
                for (const auto& recommendation : bottleneck.m_Recommendations)
                {
                    ImGui::BulletText("%s", recommendation.c_str());
                }
            }
        }
        else
        {
            ImGui::Text("Automatic bottleneck analysis is disabled.");
            ImGui::Text("Enable it in the Options menu to see analysis.");
        }
        
        // Manual analysis tools
        ImGui::Separator();
        ImGui::Text("Manual Analysis Tools:");
        
        if (ImGui::Button("Analyze Current Frame"))
        {
            // Trigger manual analysis
        }
        
        if (ImGui::Button("Compare with Previous Frame"))
        {
            // Compare frames
        }
    }
    
    void RendererProfiler::RenderCountersTab()
    {
        ImGui::Text("Performance Counters:");
        
        if (ImGui::BeginTable("CountersTable", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Average", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();
            
            for (u32 i = 0; i < (u32)MetricType::COUNT; ++i)
            {
                MetricType type = (MetricType)i;
                const auto& counter = m_Counters.at(type);
                
                ImGui::TableNextRow();
                
                ImGui::TableSetColumnIndex(0);
                ImVec4 color = GetMetricTypeColor(type);
                ImGui::TextColored(color, "%s", GetMetricTypeName(type).c_str());
                
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.2f %s", counter.m_Value, GetMetricTypeUnit(type).c_str());
                
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f %s", counter.m_Average, GetMetricTypeUnit(type).c_str());
                
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.2f %s", counter.m_Min == DBL_MAX ? 0.0 : counter.m_Min, GetMetricTypeUnit(type).c_str());
                
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.2f %s", counter.m_Max, GetMetricTypeUnit(type).c_str());
                
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%u", counter.m_SampleCount);
            }
            
            ImGui::EndTable();
        }
    }
    
    void RendererProfiler::RenderHistoryTab()
    {
        ImGui::Text("Performance History (Last %d frames):", OLO_FRAME_HISTORY_SIZE);
        
        // Extract data for plotting
        std::vector<f32> frameTimeData(OLO_FRAME_HISTORY_SIZE);
        std::vector<f32> drawCallData(OLO_FRAME_HISTORY_SIZE);
        std::vector<f32> cpuTimeData(OLO_FRAME_HISTORY_SIZE);
        std::vector<f32> gpuTimeData(OLO_FRAME_HISTORY_SIZE);
        
        for (u32 i = 0; i < OLO_FRAME_HISTORY_SIZE; ++i)
        {
            u32 index = (m_HistoryIndex + i) % OLO_FRAME_HISTORY_SIZE;
            frameTimeData[i] = (f32)m_FrameHistory[index].m_FrameTime;
            drawCallData[i] = (f32)m_FrameHistory[index].m_DrawCalls;
            cpuTimeData[i] = (f32)m_FrameHistory[index].m_CPUTime;
            gpuTimeData[i] = (f32)m_FrameHistory[index].m_GPUTime;
        }
        
        // Plot graphs
        ImGui::PlotLines("Frame Time (ms)", frameTimeData.data(), OLO_FRAME_HISTORY_SIZE, 0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 80));
        ImGui::PlotLines("CPU Time (ms)", cpuTimeData.data(), OLO_FRAME_HISTORY_SIZE, 0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
        
        if (m_EnableGPUTiming)
        {
            ImGui::PlotLines("GPU Time (ms)", gpuTimeData.data(), OLO_FRAME_HISTORY_SIZE, 0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
        }
        
        ImGui::PlotLines("Draw Calls", drawCallData.data(), OLO_FRAME_HISTORY_SIZE, 0, nullptr, 0.0f, FLT_MAX, ImVec2(0, 60));
    }
    
    RendererProfiler::BottleneckInfo RendererProfiler::AnalyzeBottlenecks() const
    {
        BottleneckInfo info;
        
        // Simple heuristic-based analysis
        f64 frameTime = m_CurrentFrame.m_FrameTime;
        f64 cpuTime = m_CurrentFrame.m_CPUTime;
        f64 gpuTime = m_CurrentFrame.m_GPUTime;
        
        if (frameTime <= 0.0)
        {
            info.m_Type = BottleneckInfo::Balanced;
            info.m_Confidence = 0.0f;
            info.m_Description = "No performance data available";
            return info;
        }
        
        f64 cpuUtilization = cpuTime / frameTime;
        f64 gpuUtilization = gpuTime / frameTime;
        
        // Determine primary bottleneck
        if (!m_EnableGPUTiming || gpuTime <= 0.0)
        {
            // Can't determine GPU usage, assume CPU bound if frame time is high
            if (frameTime > (1000.0 / m_TargetFrameRate))
            {
                info.m_Type = BottleneckInfo::CPU_Bound;
                info.m_Confidence = 0.7f;
                info.m_Description = "Frame time exceeds target. Enable GPU timing for better analysis.";
            }
            else
            {
                info.m_Type = BottleneckInfo::Balanced;
                info.m_Confidence = 0.8f;
                info.m_Description = "Performance appears balanced.";
            }
        }
        else
        {
            if (cpuUtilization > 0.8 && cpuUtilization > gpuUtilization)
            {
                info.m_Type = BottleneckInfo::CPU_Bound;
                info.m_Confidence = (f32)std::min(cpuUtilization, 1.0);
                info.m_Description = "CPU is the primary bottleneck. Consider optimizing CPU-side rendering logic.";
                info.m_Recommendations = {
                    "Reduce draw calls through batching",
                    "Optimize culling algorithms",
                    "Minimize state changes",
                    "Use instanced rendering for similar objects"
                };
            }
            else if (gpuUtilization > 0.8 && gpuUtilization > cpuUtilization)
            {
                info.m_Type = BottleneckInfo::GPU_Bound;
                info.m_Confidence = (f32)std::min(gpuUtilization, 1.0);
                info.m_Description = "GPU is the primary bottleneck. Consider optimizing shaders or reducing scene complexity.";
                info.m_Recommendations = {
                    "Optimize shader performance",
                    "Reduce texture resolution or compression",
                    "Implement level-of-detail (LOD) systems",
                    "Use occlusion culling"
                };
            }
            else
            {
                info.m_Type = BottleneckInfo::Balanced;
                info.m_Confidence = 0.8f;
                info.m_Description = "CPU and GPU utilization appears balanced.";
            }
        }
        
        return info;
    }
    
    f32 RendererProfiler::CalculateFrameRate() const
    {
        if (m_CurrentFrame.m_FrameTime <= 0.0)
            return 0.0f;
        return (f32)(1000.0 / m_CurrentFrame.m_FrameTime);
    }
    
    f32 RendererProfiler::CalculateAverageFrameTime() const
    {
        return (f32)m_Counters.at(MetricType::FrameTime).m_Average;
    }
    
    std::string RendererProfiler::GetMetricTypeName(MetricType type) const
    {
        switch (type)
        {
            case MetricType::FrameTime:         return "Frame Time";
            case MetricType::CPUTime:           return "CPU Time";
            case MetricType::GPUTime:           return "GPU Time";
            case MetricType::DrawCalls:         return "Draw Calls";
            case MetricType::StateChanges:      return "State Changes";
            case MetricType::ShaderBinds:       return "Shader Binds";
            case MetricType::TextureBinds:      return "Texture Binds";
            case MetricType::BufferBinds:       return "Buffer Binds";
            case MetricType::VerticesRendered:  return "Vertices";
            case MetricType::TrianglesRendered: return "Triangles";
            case MetricType::CommandPackets:    return "Command Packets";
            case MetricType::SortingTime:       return "Sorting Time";
            case MetricType::CullingTime:       return "Culling Time";
            default:                            return "Unknown";
        }
    }
    
    std::string RendererProfiler::GetMetricTypeUnit(MetricType type) const
    {
        switch (type)
        {
            case MetricType::FrameTime:
            case MetricType::CPUTime:
            case MetricType::GPUTime:
            case MetricType::SortingTime:
            case MetricType::CullingTime:
                return "ms";
            default:
                return "";
        }
    }
    
    ImVec4 RendererProfiler::GetMetricTypeColor(MetricType type) const
    {
        switch (type)
        {
            case MetricType::FrameTime:         return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            case MetricType::CPUTime:           return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            case MetricType::GPUTime:           return ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
            case MetricType::DrawCalls:         return ImVec4(0.2f, 0.6f, 0.8f, 1.0f);
            case MetricType::StateChanges:      return ImVec4(0.8f, 0.6f, 0.2f, 1.0f);
            case MetricType::ShaderBinds:       return ImVec4(0.8f, 0.2f, 0.8f, 1.0f);
            case MetricType::TextureBinds:      return ImVec4(0.6f, 0.2f, 0.8f, 1.0f);
            case MetricType::BufferBinds:       return ImVec4(0.2f, 0.8f, 0.8f, 1.0f);
            case MetricType::VerticesRendered:  return ImVec4(0.6f, 0.8f, 0.2f, 1.0f);
            case MetricType::TrianglesRendered: return ImVec4(0.8f, 0.8f, 0.2f, 1.0f);
            case MetricType::CommandPackets:    return ImVec4(0.4f, 0.6f, 0.8f, 1.0f);
            case MetricType::SortingTime:       return ImVec4(0.8f, 0.4f, 0.6f, 1.0f);
            case MetricType::CullingTime:       return ImVec4(0.6f, 0.8f, 0.4f, 1.0f);
            default:                            return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        }
    }
    
    bool RendererProfiler::ExportToCSV(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();
        
        try
        {
            std::ofstream file(filePath);
            if (!file.is_open())
                return false;
            
            // CSV header
            file << "Frame,FrameTime,CPUTime,GPUTime,DrawCalls,StateChanges,ShaderBinds,TextureBinds,BufferBinds,Vertices,Triangles,CommandPackets,SortingTime,CullingTime\n";
            
            // Export frame history
            for (u32 i = 0; i < OLO_FRAME_HISTORY_SIZE; ++i)
            {
                u32 index = (m_HistoryIndex + i) % OLO_FRAME_HISTORY_SIZE;
                const FrameData& frame = m_FrameHistory[index];
                
                file << i << ","
                     << frame.m_FrameTime << ","
                     << frame.m_CPUTime << ","
                     << frame.m_GPUTime << ","
                     << frame.m_DrawCalls << ","
                     << frame.m_StateChanges << ","
                     << frame.m_ShaderBinds << ","
                     << frame.m_TextureBinds << ","
                     << frame.m_BufferBinds << ","
                     << frame.m_VerticesRendered << ","
                     << frame.m_TrianglesRendered << ","
                     << frame.m_CommandPackets << ","
                     << frame.m_SortingTime << ","
                     << frame.m_CullingTime << "\n";
            }
            
            file.close();
            OLO_CORE_INFO("Performance data exported to: {0}", filePath);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to export performance data: {0}", e.what());
            return false;
        }
    }
    
    bool RendererProfiler::IsHittingTargetFramerate(f32 targetFPS) const
    {
        f32 currentFPS = CalculateFrameRate();
        return currentFPS >= (targetFPS * 0.95f); // 5% tolerance
    }

    void RendererProfiler::PerformanceCounter::AddSample(f64 value)
    {
        m_Value = value;
        m_SampleCount++;
        
        if (value < m_Min)
            m_Min = value;
        if (value > m_Max)
            m_Max = value;
        
        // Update running average
        m_Average = ((m_Average * (m_SampleCount - 1)) + value) / m_SampleCount;
        
        // Initialize history buffer on first use
        if (m_History.empty())
        {
            m_History.resize(OLO_HISTORY_SIZE, 0.0f);
        }
        
        // Add to ring buffer history - O(1) operation
        m_History[m_HistoryIndex] = (f32)value;
        m_HistoryIndex = (m_HistoryIndex + 1) % OLO_HISTORY_SIZE;
        
        // Track how many valid samples we have (up to buffer size)
        if (m_HistoryCount < OLO_HISTORY_SIZE)
            m_HistoryCount++;
    }
      void RendererProfiler::PerformanceCounter::Reset()
    {
        m_Value = 0.0;
        m_Min = DBL_MAX;
        m_Max = 0.0;
        m_Average = 0.0;
        m_SampleCount = 0;
        m_History.clear();
        m_HistoryIndex = 0;
        m_HistoryCount = 0;
    }
    
    void RendererProfiler::PerformanceCounter::GetHistoryInOrder(std::vector<f32>& outHistory) const
    {
        if (m_HistoryCount == 0)
        {
            outHistory.clear();
            return;
        }
        
        outHistory.resize(m_HistoryCount);
        
        // Copy data in chronological order (oldest to newest)
        for (u32 i = 0; i < m_HistoryCount; ++i)
        {
            u32 sourceIndex;
            if (m_HistoryCount < OLO_HISTORY_SIZE)
            {
                // Buffer not yet full, data starts from index 0
                sourceIndex = i;
            }
            else
            {
                // Buffer is full, start from the oldest sample
                sourceIndex = (m_HistoryIndex + i) % OLO_HISTORY_SIZE;
            }
            outHistory[i] = m_History[sourceIndex];
        }
    }
    
    void RendererProfiler::FrameData::Reset()
    {
        m_FrameTime = 0.0;
        m_CPUTime = 0.0;
        m_GPUTime = 0.0;
        m_DrawCalls = 0;
        m_StateChanges = 0;
        m_ShaderBinds = 0;
        m_TextureBinds = 0;
        m_BufferBinds = 0;
        m_VerticesRendered = 0;
        m_TrianglesRendered = 0;
        m_CommandPackets = 0;
        m_SortingTime = 0.0;
        m_CullingTime = 0.0;
    }
    
    // ProfileScope implementation
    RendererProfiler::ProfileScope::ProfileScope(const std::string& name, MetricType type)
        : m_Name(name), m_Type(type)
    {
        m_StartTime = std::chrono::high_resolution_clock::now();
    }
    
    RendererProfiler::ProfileScope::~ProfileScope()
    {
        auto endTime = std::chrono::high_resolution_clock::now();
        f64 duration = std::chrono::duration<f64, std::milli>(endTime - m_StartTime).count();
        RendererProfiler::GetInstance().AddTimingSample(m_Name, duration, m_Type);
    }

    // Frame Capture Implementation
    void RendererProfiler::CaptureFrame(const std::string& notes)
    {
        OLO_PROFILE_FUNCTION();
        
        // Don't capture if we're already capturing
        if (m_CapturingFrame)
        {
            OLO_CORE_WARN("RendererProfiler: Already capturing a frame, ignoring request");
            return;
        }
        
        CapturedFrame frame;
        frame.m_FrameNumber = m_FrameNumber;
        frame.m_Timestamp = std::chrono::duration<f64>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        frame.m_FrameData = m_CurrentFrame;
        frame.m_Notes = notes;
        frame.m_BottleneckAnalysis = AnalyzeBottlenecks();
        
        // Add to captured frames (keep only last N frames)
        m_CapturedFrames.push_back(frame);
        if (m_CapturedFrames.size() > OLO_MAX_CAPTURED_FRAMES)
        {
            m_CapturedFrames.erase(m_CapturedFrames.begin());
        }
        
        OLO_CORE_INFO("RendererProfiler: Captured frame {} - {}", frame.m_FrameNumber, notes);
    }

    void RendererProfiler::BeginRenderPass(const std::string& passName)
    {
        if (!m_CapturingFrame)
            return;
            
        RenderPassInfo passInfo;
        passInfo.m_Name = passName;
        passInfo.m_StartTime = std::chrono::duration<f64, std::milli>(
            std::chrono::high_resolution_clock::now() - m_FrameStartTime).count();
        
        // If we have a current captured frame, add this pass to it
        if (!m_CapturedFrames.empty())
        {
            m_CapturedFrames.back().m_RenderPasses.push_back(passInfo);
            m_CurrentRenderPass = &m_CapturedFrames.back().m_RenderPasses.back();
        }
        
        OLO_CORE_TRACE("RendererProfiler: Begin render pass '{}'", passName);
    }

    void RendererProfiler::EndRenderPass()
    {
        if (!m_CapturingFrame || !m_CurrentRenderPass)
            return;
            
        f64 currentTime = std::chrono::duration<f64, std::milli>(
            std::chrono::high_resolution_clock::now() - m_FrameStartTime).count();
        m_CurrentRenderPass->m_Duration = currentTime - m_CurrentRenderPass->m_StartTime;
        
        OLO_CORE_TRACE("RendererProfiler: End render pass '{}' ({}ms)", 
                      m_CurrentRenderPass->m_Name, m_CurrentRenderPass->m_Duration);
        
        m_CurrentRenderPass = nullptr;
    }

    void RendererProfiler::TrackDrawCall(const std::string& name, const std::string& shaderName, 
                                        u32 vertexCount, u32 indexCount, f64 cpuTime, f64 gpuTime)
    {
        if (!m_CapturingFrame || !m_CurrentRenderPass)
            return;
            
        DrawCallInfo drawCall;
        drawCall.m_Name = name;
        drawCall.m_ShaderName = shaderName;
        drawCall.m_VertexCount = vertexCount;
        drawCall.m_IndexCount = indexCount;
        drawCall.m_CPUTime = cpuTime;
        drawCall.m_GPUTime = gpuTime;
        
        m_CurrentRenderPass->m_DrawCalls.push_back(drawCall);
        m_CurrentRenderPass->m_DrawCallCount++;
        
        OLO_CORE_TRACE("RendererProfiler: Tracked draw call '{}' with shader '{}' - {} verts, {} indices", 
                      name, shaderName, vertexCount, indexCount);
    }

    std::string RendererProfiler::CompareFrames(const CapturedFrame& frame1, const CapturedFrame& frame2) const
    {
        std::stringstream ss;
        
        ss << "Frame Comparison:\n";
        ss << "================\n\n";
        
        ss << "Frame " << frame1.m_FrameNumber << " vs Frame " << frame2.m_FrameNumber << "\n\n";
        
        // Frame time comparison
        f64 frameTimeDiff = frame2.m_FrameData.m_FrameTime - frame1.m_FrameData.m_FrameTime;
        ss << "Frame Time: " << frame1.m_FrameData.m_FrameTime << "ms -> " 
           << frame2.m_FrameData.m_FrameTime << "ms ";
        if (frameTimeDiff > 0)
            ss << "(+" << frameTimeDiff << "ms SLOWER)\n";
        else
            ss << "(" << frameTimeDiff << "ms faster)\n";
        
        // Draw call comparison
        i32 drawCallDiff = (i32)frame2.m_FrameData.m_DrawCalls - (i32)frame1.m_FrameData.m_DrawCalls;
        ss << "Draw Calls: " << frame1.m_FrameData.m_DrawCalls << " -> " 
           << frame2.m_FrameData.m_DrawCalls;
        if (drawCallDiff != 0)
            ss << " (" << (drawCallDiff > 0 ? "+" : "") << drawCallDiff << ")";
        ss << "\n";
        
        // Vertices comparison
        i32 vertexDiff = (i32)frame2.m_FrameData.m_VerticesRendered - (i32)frame1.m_FrameData.m_VerticesRendered;
        ss << "Vertices: " << frame1.m_FrameData.m_VerticesRendered << " -> " 
           << frame2.m_FrameData.m_VerticesRendered;
        if (vertexDiff != 0)
            ss << " (" << (vertexDiff > 0 ? "+" : "") << vertexDiff << ")";
        ss << "\n";
        
        // Bottleneck analysis
        ss << "\nBottleneck Analysis:\n";
        ss << "Frame " << frame1.m_FrameNumber << ": " << frame1.m_BottleneckAnalysis.m_Description << "\n";
        ss << "Frame " << frame2.m_FrameNumber << ": " << frame2.m_BottleneckAnalysis.m_Description << "\n";
        
        return ss.str();
    }
    
    void RendererProfiler::RenderFrameCaptureTab()
    {
        ImGui::Text("Frame Capture & Analysis");
        ImGui::Separator();
        
        // Capture controls
        ImGui::Text("Current Frame: %u", m_FrameNumber);
        
        static char captureNotes[256] = "";
        ImGui::InputText("Notes", captureNotes, sizeof(captureNotes));
        ImGui::SameLine();
        
        if (ImGui::Button("Capture Current Frame"))
        {
            CaptureFrame(std::string(captureNotes));
            captureNotes[0] = '\0'; // Clear notes
        }
        
        ImGui::Separator();
        
        // Display captured frames
        ImGui::Text("Captured Frames: %zu", m_CapturedFrames.size());
        
        if (ImGui::Button("Clear All Captures"))
        {
            ClearCapturedFrames();
        }
        
        // Frame list
        if (!m_CapturedFrames.empty())
        {
            if (ImGui::BeginTable("CapturedFrames", 6, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Frame #", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Frame Time", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Draw Calls", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Bottleneck", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                
                for (const auto& frame : m_CapturedFrames)
                {
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%u", frame.m_FrameNumber);
                    
                    ImGui::TableSetColumnIndex(1);
                    // Color code frame times
                    if (frame.m_FrameData.m_FrameTime > 16.67f) // < 60 FPS
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%.2fms", frame.m_FrameData.m_FrameTime);
                    else if (frame.m_FrameData.m_FrameTime > 11.11f) // < 90 FPS
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%.2fms", frame.m_FrameData.m_FrameTime);
                    else
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.2fms", frame.m_FrameData.m_FrameTime);
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", frame.m_FrameData.m_DrawCalls);
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", frame.m_FrameData.m_VerticesRendered);
                    
                    ImGui::TableSetColumnIndex(4);
                    switch (frame.m_BottleneckAnalysis.m_Type)
                    {
                        case BottleneckInfo::CPU_Bound:
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "CPU");
                            break;
                        case BottleneckInfo::GPU_Bound:
                            ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "GPU");
                            break;
                        case BottleneckInfo::Memory_Bound:
                            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Memory");
                            break;
                        case BottleneckInfo::IO_Bound:
                            ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "I/O");
                            break;
                        default:
                            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "Balanced");
                            break;
                    }
                    
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%s", frame.m_Notes.c_str());
                }
                
                ImGui::EndTable();
            }
        }
        else
        {
            ImGui::Text("No frames captured yet. Use 'Capture Current Frame' to start analyzing.");
        }
    }

    void RendererProfiler::RenderFrameComparisonTab()
    {
        ImGui::Text("Frame Comparison Tool");
        ImGui::Separator();
        
        if (m_CapturedFrames.size() < 2)
        {
            ImGui::Text("Capture at least 2 frames to enable comparison.");
            return;
        }
        
        static int selectedFrame1 = 0;
        static int selectedFrame2 = 1;
        
        // Frame selection
        ImGui::Text("Select frames to compare:");
        
        // Build frame list for combo boxes
        std::vector<std::string> frameNames;
        for (sizet i = 0; i < m_CapturedFrames.size(); ++i)
        {
            const auto& frame = m_CapturedFrames[i];
            std::string name = "Frame " + std::to_string(frame.m_FrameNumber) + 
                              " (" + std::to_string(frame.m_FrameData.m_FrameTime) + "ms)";
            if (!frame.m_Notes.empty())
                name += " - " + frame.m_Notes;
            frameNames.push_back(name);
        }
        
        // Convert to char* array for ImGui
        std::vector<const char*> frameNamePtrs;
        for (const auto& name : frameNames)
            frameNamePtrs.push_back(name.c_str());
        
        ImGui::Combo("Frame 1", &selectedFrame1, frameNamePtrs.data(), (int)frameNamePtrs.size());
        ImGui::Combo("Frame 2", &selectedFrame2, frameNamePtrs.data(), (int)frameNamePtrs.size());
        
        if (selectedFrame1 >= 0 && selectedFrame1 < (int)m_CapturedFrames.size() &&
            selectedFrame2 >= 0 && selectedFrame2 < (int)m_CapturedFrames.size() &&
            selectedFrame1 != selectedFrame2)
        {
            const auto& frame1 = m_CapturedFrames[selectedFrame1];
            const auto& frame2 = m_CapturedFrames[selectedFrame2];
            
            ImGui::Separator();
            
            // Quick comparison metrics
            if (ImGui::BeginTable("FrameComparison", 3, ImGuiTableFlags_Borders))
            {
                ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Frame 1", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Frame 2", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableHeadersRow();
                
                // Frame time
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("Frame Time");
                ImGui::TableSetColumnIndex(1); 
                ImGui::Text("%.2fms", frame1.m_FrameData.m_FrameTime);
                ImGui::TableSetColumnIndex(2);
                f64 frameTimeDiff = frame2.m_FrameData.m_FrameTime - frame1.m_FrameData.m_FrameTime;
                if (frameTimeDiff > 0.5f)
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%.2fms (+%.2f)", frame2.m_FrameData.m_FrameTime, frameTimeDiff);
                else if (frameTimeDiff < -0.5f)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%.2fms (%.2f)", frame2.m_FrameData.m_FrameTime, frameTimeDiff);
                else
                    ImGui::Text("%.2fms", frame2.m_FrameData.m_FrameTime);
                
                // Draw calls
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("Draw Calls");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%u", frame1.m_FrameData.m_DrawCalls);
                ImGui::TableSetColumnIndex(2);
                i32 drawCallDiff = (i32)frame2.m_FrameData.m_DrawCalls - (i32)frame1.m_FrameData.m_DrawCalls;
                if (drawCallDiff > 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%u (+%d)", frame2.m_FrameData.m_DrawCalls, drawCallDiff);
                else if (drawCallDiff < 0)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u (%d)", frame2.m_FrameData.m_DrawCalls, drawCallDiff);
                else
                    ImGui::Text("%u", frame2.m_FrameData.m_DrawCalls);
                
                // Vertices
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("Vertices");
                ImGui::TableSetColumnIndex(1); ImGui::Text("%u", frame1.m_FrameData.m_VerticesRendered);
                ImGui::TableSetColumnIndex(2);
                i32 vertexDiff = (i32)frame2.m_FrameData.m_VerticesRendered - (i32)frame1.m_FrameData.m_VerticesRendered;
                if (vertexDiff > 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%u (+%d)", frame2.m_FrameData.m_VerticesRendered, vertexDiff);
                else if (vertexDiff < 0)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%u (%d)", frame2.m_FrameData.m_VerticesRendered, vertexDiff);
                else
                    ImGui::Text("%u", frame2.m_FrameData.m_VerticesRendered);
                
                ImGui::EndTable();
            }
            
            ImGui::Separator();
            
            // Detailed comparison text
            if (ImGui::Button("Generate Detailed Report"))
            {
                std::string report = CompareFrames(frame1, frame2);
                // For now just log it, later we could show it in a popup or export it
                OLO_CORE_INFO("Frame Comparison Report:\n{}", report);
            }
        }
    }
}
