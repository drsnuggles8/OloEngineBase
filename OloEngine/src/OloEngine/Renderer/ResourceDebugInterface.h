#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ResourceStateTracker.h"
#include "OloEngine/Renderer/ResourcePool.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"

#include <unordered_map>
#include <vector>
#include <functional>

namespace OloEngine
{
    /**
     * @brief Advanced debug interface for resource management system
     * 
     * Provides comprehensive debugging and profiling tools for the resource management system:
     * - Resource usage visualization and analytics
     * - Pool statistics and health monitoring
     * - Resource state tracking and history
     * - Performance bottleneck identification
     * - Memory usage analysis and leak detection
     */
    class ResourceDebugInterface
    {
    public:
        struct ResourceMetrics
        {
            std::string Name;
            ShaderResourceType Type;
            sizet MemoryUsage = 0;
            u64 AccessCount = 0;
            u64 BindingCount = 0;
            u64 UpdateCount = 0;
            bool IsHot = false;
            bool IsDirty = false;
            f64 UtilizationScore = 0.0; // 0.0 = never used, 1.0 = heavily used
        };

        struct PoolMetrics
        {
            std::string PoolName;
            std::string ResourceType;
            u32 TotalResources = 0;
            u32 AvailableResources = 0;
            u32 InUseResources = 0;
            f32 UtilizationRatio = 0.0f;
            u32 TotalCreated = 0;
            u32 TotalAcquired = 0;
            u32 TotalReleased = 0;
            u32 ValidationFailures = 0;
        };

        struct MemoryMetrics
        {
            sizet TotalAllocated = 0;
            sizet TotalInUse = 0;
            sizet TotalCached = 0;
            u32 AllocationCount = 0;
            u32 DeallocationCount = 0;
            u32 CacheHits = 0;
            u32 CacheMisses = 0;
            f64 CacheHitRatio = 0.0;
            f64 MemoryEfficiency = 0.0; // In use / Allocated
        };

        struct PerformanceMetrics
        {
            f64 AverageAccessTime = 0.0; // Microseconds
            f64 AverageBindingTime = 0.0;
            f64 AverageUpdateTime = 0.0;
            u32 StateChangesPerFrame = 0;
            u32 ResourceBindingsPerFrame = 0;
            u32 RedundantBindings = 0; // Same resource bound multiple times
            f64 FrameTimeImpact = 0.0; // Percentage of frame time spent on resource management
        };

    private:
        ResourceStateTracker* m_StateTracker = nullptr;
        std::unordered_map<std::string, void*> m_ResourcePools; // Type-erased pool references
        std::unordered_map<std::string, std::function<PoolMetrics()>> m_PoolMetricsGetters;
        
        // Debug visualization state
        bool m_ShowResourceList = true;
        bool m_ShowPoolStatistics = true;
        bool m_ShowMemoryAnalysis = true;
        bool m_ShowPerformanceMetrics = true;
        bool m_ShowHeatMap = false;
        
        // Filtering and sorting
        int m_ResourceTypeFilter = -1; // -1 = All
        int m_SortMode = 0; // 0 = Name, 1 = Memory, 2 = Access Count
        bool m_SortDescending = false;
        bool m_ShowOnlyHotResources = false;
        bool m_ShowOnlyDirtyResources = false;
        
        // Performance tracking
        mutable std::vector<f64> m_FrameTimeHistory;
        mutable sizet m_MaxHistorySize = 120; // 2 seconds at 60 FPS

    public:
        ResourceDebugInterface() = default;
        ~ResourceDebugInterface() = default;

        /**
         * @brief Set the resource state tracker to monitor
         */
        void SetStateTracker(ResourceStateTracker* tracker)
        {
            m_StateTracker = tracker;
        }

        /**
         * @brief Register a resource pool for monitoring
         */
        template<typename ResourceType>
        void RegisterPool(const std::string& name, ResourcePool<ResourceType>* pool)
        {
            m_ResourcePools[name] = static_cast<void*>(pool);
            
            // Create metrics getter for this pool type
            m_PoolMetricsGetters[name] = [pool]() -> PoolMetrics {
                auto stats = pool->GetStatistics();
                PoolMetrics metrics;
                metrics.PoolName = "Pool"; // We don't have access to the name here
                metrics.ResourceType = typeid(ResourceType).name();
                metrics.TotalResources = stats.TotalCount;
                metrics.AvailableResources = stats.AvailableCount;
                metrics.InUseResources = stats.InUseCount;
                metrics.UtilizationRatio = stats.UtilizationRatio;
                metrics.TotalCreated = stats.TotalCreated;
                metrics.TotalAcquired = stats.TotalAcquired;
                metrics.TotalReleased = stats.TotalReleased;
                metrics.ValidationFailures = stats.ValidationFailures;
                return metrics;
            };
        }

        /**
         * @brief Unregister a resource pool
         */
        void UnregisterPool(const std::string& name)
        {
            m_ResourcePools.erase(name);
            m_PoolMetricsGetters.erase(name);
        }

        /**
         * @brief Get resource metrics for all tracked resources
         */
        std::vector<ResourceMetrics> GetResourceMetrics() const
        {
            std::vector<ResourceMetrics> metrics;
            
            if (!m_StateTracker)
                return metrics;
            
            auto resources = m_StateTracker->GetAllResources();
            metrics.reserve(resources.size());
            
            for (const auto& resource : resources)
            {
                ResourceMetrics metric;
                metric.Name = resource.Name;
                metric.Type = resource.Type;
                metric.MemoryUsage = resource.MemoryUsage;
                metric.AccessCount = resource.TotalAccesses;
                metric.BindingCount = resource.TotalBindings;
                metric.UpdateCount = resource.TotalUpdates;
                metric.IsHot = resource.IsHot;
                metric.IsDirty = resource.IsDirty;
                
                // Calculate utilization score (simple heuristic)
                if (resource.TotalAccesses > 0)
                {
                    metric.UtilizationScore = std::min(1.0, static_cast<f64>(resource.TotalAccesses) / 100.0);
                }
                
                metrics.push_back(metric);
            }
            
            return metrics;
        }

        /**
         * @brief Get pool metrics for all registered pools
         */
        std::vector<PoolMetrics> GetPoolMetrics() const
        {
            std::vector<PoolMetrics> metrics;
            metrics.reserve(m_PoolMetricsGetters.size());
            
            for (const auto& [name, getter] : m_PoolMetricsGetters)
            {
                auto poolMetrics = getter();
                poolMetrics.PoolName = name;
                metrics.push_back(poolMetrics);
            }
            
            return metrics;
        }

        /**
         * @brief Get memory usage metrics
         */
        MemoryMetrics GetMemoryMetrics() const
        {
            MemoryMetrics metrics;
            
            // Aggregate from resource metrics
            auto resourceMetrics = GetResourceMetrics();
            for (const auto& resource : resourceMetrics)
            {
                metrics.TotalAllocated += resource.MemoryUsage;
                if (resource.AccessCount > 0)
                {
                    metrics.TotalInUse += resource.MemoryUsage;
                }
            }
            
            // Aggregate from pool metrics
            auto poolMetrics = GetPoolMetrics();
            for (const auto& pool : poolMetrics)
            {
                metrics.AllocationCount += pool.TotalCreated;
                // Approximate cache metrics based on pool utilization
                if (pool.TotalAcquired > 0)
                {
                    metrics.CacheHits += static_cast<u32>(pool.TotalAcquired * pool.UtilizationRatio);
                    metrics.CacheMisses += pool.TotalAcquired - metrics.CacheHits;
                }
            }
            
            // Calculate derived metrics
            if (metrics.TotalAllocated > 0)
            {
                metrics.MemoryEfficiency = static_cast<f64>(metrics.TotalInUse) / metrics.TotalAllocated;
            }
            
            if (metrics.CacheHits + metrics.CacheMisses > 0)
            {
                metrics.CacheHitRatio = static_cast<f64>(metrics.CacheHits) / (metrics.CacheHits + metrics.CacheMisses);
            }
            
            return metrics;
        }

        /**
         * @brief Get performance metrics
         */
        PerformanceMetrics GetPerformanceMetrics() const
        {
            PerformanceMetrics metrics;
            
            if (!m_StateTracker)
                return metrics;
            
            auto frameStats = m_StateTracker->GetCurrentFrameStats();
            
            metrics.StateChangesPerFrame = static_cast<u32>(frameStats.TotalStateChanges);
            metrics.ResourceBindingsPerFrame = static_cast<u32>(frameStats.TotalResourceBindings);
            
            // Calculate frame time impact (simplified)
            if (!m_FrameTimeHistory.empty())
            {
                f64 avgFrameTime = 0.0;
                for (f64 time : m_FrameTimeHistory)
                {
                    avgFrameTime += time;
                }
                avgFrameTime /= m_FrameTimeHistory.size();
                
                // Estimate resource management overhead (very rough approximation)
                f64 resourceOverhead = (frameStats.TotalResourceBindings * 0.001) + (frameStats.TotalStateChanges * 0.0005);
                metrics.FrameTimeImpact = (resourceOverhead / avgFrameTime) * 100.0;
            }
            
            return metrics;
        }

        /**
         * @brief Render the debug interface using ImGui
         */
        void RenderDebugInterface()
        {
            if (!ImGui::Begin("Resource Management Debug"))
            {
                ImGui::End();
                return;
            }
            
            // Main tabs
            if (ImGui::BeginTabBar("ResourceDebugTabs"))
            {
                if (ImGui::BeginTabItem("Resources"))
                {
                    RenderResourceList();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Pools"))
                {
                    RenderPoolStatistics();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Memory"))
                {
                    RenderMemoryAnalysis();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Performance"))
                {
                    RenderPerformanceMetrics();
                    ImGui::EndTabItem();
                }
                
                ImGui::EndTabBar();
            }
            
            ImGui::End();
        }

        /**
         * @brief Update performance tracking (call each frame)
         */
        void UpdatePerformanceTracking(f64 frameTime)
        {
            m_FrameTimeHistory.push_back(frameTime);
            
            if (m_FrameTimeHistory.size() > m_MaxHistorySize)
            {
                m_FrameTimeHistory.erase(m_FrameTimeHistory.begin());
            }
            
            if (m_StateTracker)
            {
                m_StateTracker->NextFrame();
            }
        }

    private:
        void RenderResourceList()
        {
            auto resources = GetResourceMetrics();
            
            // Filtering controls
            ImGui::Text("Resource List (%zu resources)", resources.size());
            ImGui::Separator();
            
            const char* typeItems[] = { "All", "UBO", "SSBO", "Tex2D", "TexCube", "UBO[]", "SSBO[]", "Tex2D[]", "TexCube[]" };
            ImGui::Combo("Type Filter", &m_ResourceTypeFilter, typeItems, IM_ARRAYSIZE(typeItems));
            
            ImGui::SameLine();
            const char* sortItems[] = { "Name", "Memory Usage", "Access Count", "Utilization" };
            ImGui::Combo("Sort By", &m_SortMode, sortItems, IM_ARRAYSIZE(sortItems));
            
            ImGui::SameLine();
            ImGui::Checkbox("Descending", &m_SortDescending);
            
            ImGui::Checkbox("Show Only Hot", &m_ShowOnlyHotResources);
            ImGui::SameLine();
            ImGui::Checkbox("Show Only Dirty", &m_ShowOnlyDirtyResources);
            
            ImGui::Separator();
            
            // Filter and sort resources
            auto filteredResources = FilterAndSortResources(resources);
            
            // Resource table
            if (ImGui::BeginTable("ResourceTable", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Memory");
                ImGui::TableSetupColumn("Accesses");
                ImGui::TableSetupColumn("Bindings");
                ImGui::TableSetupColumn("Updates");
                ImGui::TableSetupColumn("Utilization");
                ImGui::TableSetupColumn("Status");
                ImGui::TableHeadersRow();
                
                for (const auto& resource : filteredResources)
                {
                    ImGui::TableNextRow();
                    
                    ImGui::TableSetColumnIndex(0);
                    if (resource.IsHot)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", resource.Name.c_str());
                    }
                    else
                    {
                        ImGui::Text("%s", resource.Name.c_str());
                    }
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", GetResourceTypeString(resource.Type));
                    
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.2f KB", static_cast<f32>(resource.MemoryUsage) / 1024.0f);
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%llu", resource.AccessCount);
                    
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%llu", resource.BindingCount);
                    
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%llu", resource.UpdateCount);
                    
                    ImGui::TableSetColumnIndex(6);
                    f32 utilization = static_cast<f32>(resource.UtilizationScore);
                    ImGui::ProgressBar(utilization, ImVec2(-1, 0), "");
                    
                    ImGui::TableSetColumnIndex(7);
                    if (resource.IsDirty)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Dirty");
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Clean");
                    }
                }
                
                ImGui::EndTable();
            }
        }

        void RenderPoolStatistics()
        {
            auto pools = GetPoolMetrics();
            
            ImGui::Text("Resource Pools (%zu pools)", pools.size());
            ImGui::Separator();
            
            for (const auto& pool : pools)
            {
                if (ImGui::CollapsingHeader(pool.PoolName.c_str()))
                {
                    ImGui::Text("Resource Type: %s", pool.ResourceType.c_str());
                    ImGui::Text("Total Resources: %u", pool.TotalResources);
                    ImGui::Text("Available: %u", pool.AvailableResources);
                    ImGui::Text("In Use: %u", pool.InUseResources);
                    ImGui::Text("Utilization: %.1f%%", pool.UtilizationRatio * 100.0f);
                    
                    ImGui::Separator();
                    ImGui::Text("Statistics:");
                    ImGui::Text("  Created: %u", pool.TotalCreated);
                    ImGui::Text("  Acquired: %u", pool.TotalAcquired);
                    ImGui::Text("  Released: %u", pool.TotalReleased);
                    ImGui::Text("  Validation Failures: %u", pool.ValidationFailures);
                    
                    // Utilization bar
                    ImGui::Text("Pool Utilization:");
                    ImGui::ProgressBar(pool.UtilizationRatio, ImVec2(-1, 0), "");
                }
            }
        }

        void RenderMemoryAnalysis()
        {
            auto memory = GetMemoryMetrics();
            
            ImGui::Text("Memory Analysis");
            ImGui::Separator();
            
            ImGui::Text("Total Allocated: %.2f MB", static_cast<f32>(memory.TotalAllocated) / (1024.0f * 1024.0f));
            ImGui::Text("Total In Use: %.2f MB", static_cast<f32>(memory.TotalInUse) / (1024.0f * 1024.0f));
            ImGui::Text("Memory Efficiency: %.1f%%", memory.MemoryEfficiency * 100.0f);
            
            ImGui::Separator();
            ImGui::Text("Cache Statistics:");
            ImGui::Text("  Cache Hits: %u", memory.CacheHits);
            ImGui::Text("  Cache Misses: %u", memory.CacheMisses);
            ImGui::Text("  Hit Ratio: %.1f%%", memory.CacheHitRatio * 100.0f);
            
            // Memory efficiency bar
            ImGui::Text("Memory Efficiency:");
            ImGui::ProgressBar(static_cast<f32>(memory.MemoryEfficiency), ImVec2(-1, 0), "");
            
            // Cache hit ratio bar
            ImGui::Text("Cache Hit Ratio:");
            ImGui::ProgressBar(static_cast<f32>(memory.CacheHitRatio), ImVec2(-1, 0), "");
        }

        void RenderPerformanceMetrics()
        {
            auto performance = GetPerformanceMetrics();
            
            ImGui::Text("Performance Metrics");
            ImGui::Separator();
            
            ImGui::Text("State Changes per Frame: %u", performance.StateChangesPerFrame);
            ImGui::Text("Resource Bindings per Frame: %u", performance.ResourceBindingsPerFrame);
            ImGui::Text("Frame Time Impact: %.2f%%", performance.FrameTimeImpact);
            
            // Frame time history graph
            if (!m_FrameTimeHistory.empty())
            {
                ImGui::Text("Frame Time History:");
                ImGui::PlotLines("##FrameTime", m_FrameTimeHistory.data(), static_cast<int>(m_FrameTimeHistory.size()), 0, nullptr, 0.0f, 33.33f, ImVec2(0, 80));
            }
        }

        std::vector<ResourceMetrics> FilterAndSortResources(const std::vector<ResourceMetrics>& resources) const
        {
            std::vector<ResourceMetrics> filtered;
            
            for (const auto& resource : resources)
            {
                // Apply type filter
                if (m_ResourceTypeFilter > 0)
                {
                    int expectedType = m_ResourceTypeFilter - 1;
                    if (static_cast<int>(resource.Type) != expectedType)
                        continue;
                }
                
                // Apply hot/dirty filters
                if (m_ShowOnlyHotResources && !resource.IsHot)
                    continue;
                if (m_ShowOnlyDirtyResources && !resource.IsDirty)
                    continue;
                
                filtered.push_back(resource);
            }
            
            // Sort resources
            std::sort(filtered.begin(), filtered.end(), [this](const ResourceMetrics& a, const ResourceMetrics& b) {
                bool result = false;
                
                switch (m_SortMode)
                {
                    case 0: // Name
                        result = a.Name < b.Name;
                        break;
                    case 1: // Memory Usage
                        result = a.MemoryUsage < b.MemoryUsage;
                        break;
                    case 2: // Access Count
                        result = a.AccessCount < b.AccessCount;
                        break;
                    case 3: // Utilization
                        result = a.UtilizationScore < b.UtilizationScore;
                        break;
                }
                
                return m_SortDescending ? !result : result;
            });
            
            return filtered;
        }

        const char* GetResourceTypeString(ShaderResourceType type) const
        {
            switch (type)
            {
                case ShaderResourceType::UniformBuffer: return "UBO";
                case ShaderResourceType::StorageBuffer: return "SSBO";
                case ShaderResourceType::Texture2D: return "Tex2D";
                case ShaderResourceType::TextureCube: return "TexCube";
                case ShaderResourceType::UniformBufferArray: return "UBO[]";
                case ShaderResourceType::StorageBufferArray: return "SSBO[]";
                case ShaderResourceType::Texture2DArray: return "Tex2D[]";
                case ShaderResourceType::TextureCubeArray: return "TexCube[]";
                default: return "Unknown";
            }
        }
    };
}
