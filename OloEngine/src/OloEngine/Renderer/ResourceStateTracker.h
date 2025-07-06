#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/UniformBufferRegistry.h"

#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>

namespace OloEngine
{
    /**
     * @brief Tracks resource state changes and access patterns for optimization
     * 
     * Monitors resource usage to provide insights for performance optimization:
     * - Tracks resource binding frequency and patterns
     * - Monitors resource state changes (dirty flags, updates)
     * - Identifies hot/cold resources for caching decisions
     * - Provides memory usage and allocation tracking
     */
    class ResourceStateTracker
    {
    public:
        enum class ResourceState : u8
        {
            Created = 0,
            Bound,
            Updated,
            Dirty,
            Released,
            Cached
        };

        struct ResourceAccessInfo
        {
            std::string Name;
            ShaderResourceType Type;
            u32 ResourceID = 0;
            
            // Access statistics
            u64 TotalAccesses = 0;
            u64 TotalBindings = 0;
            u64 TotalUpdates = 0;
            u64 StateChanges = 0;
            
            // Timing information
            std::chrono::steady_clock::time_point LastAccess;
            std::chrono::steady_clock::time_point FirstAccess;
            std::chrono::steady_clock::time_point LastUpdate;
            
            // Current state
            ResourceState CurrentState = ResourceState::Created;
            bool IsDirty = false;
            bool IsHot = false; // Frequently accessed
            
            // Memory information
            sizet MemoryUsage = 0;
            u32 ReferenceCount = 0;
            
            ResourceAccessInfo() = default;
            ResourceAccessInfo(const std::string& name, ShaderResourceType type, u32 id)
                : Name(name), Type(type), ResourceID(id)
            {
                auto now = std::chrono::steady_clock::now();
                FirstAccess = now;
                LastAccess = now;
                LastUpdate = now;
            }
        };

        struct FrameStatistics
        {
            u32 FrameNumber = 0;
            u64 TotalResourceAccesses = 0;
            u64 TotalResourceBindings = 0;
            u64 TotalResourceUpdates = 0;
            u64 TotalStateChanges = 0;
            u32 UniqueResourcesAccessed = 0;
            u32 HotResourceCount = 0;
            sizet TotalMemoryUsed = 0;
            
            // Performance metrics
            f64 AverageAccessesPerResource = 0.0;
            f64 StateChangeRate = 0.0; // State changes per access
            f64 MemoryEfficiency = 0.0; // Memory used vs accessed
        };

    private:
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, ResourceAccessInfo> m_ResourceInfo;
        std::unordered_set<std::string> m_HotResources;
        
        // Frame tracking
        u32 m_CurrentFrame = 0;
        FrameStatistics m_CurrentFrameStats;
        FrameStatistics m_LastFrameStats;
        
        // Configuration
        u64 m_HotResourceThreshold = 10; // Accesses per frame to be considered "hot"
        f64 m_HotResourceDecayRate = 0.95; // How quickly hot resources cool down
        
        // Global statistics
        std::atomic<u64> m_TotalResourcesTracked{0};
        std::atomic<u64> m_TotalAccessesRecorded{0};
        std::atomic<u64> m_TotalStateChangesRecorded{0};

    public:
        ResourceStateTracker() = default;
        ~ResourceStateTracker() = default;

        // Delete copy constructor and assignment operator
        ResourceStateTracker(const ResourceStateTracker&) = delete;
        ResourceStateTracker& operator=(const ResourceStateTracker&) = delete;

        /**
         * @brief Record resource access event
         */
        void RecordAccess(const std::string& name, ShaderResourceType type, u32 resourceID = 0)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto& info = GetOrCreateResourceInfo(name, type, resourceID);
            info.TotalAccesses++;
            info.LastAccess = std::chrono::steady_clock::now();
            
            m_CurrentFrameStats.TotalResourceAccesses++;
            m_TotalAccessesRecorded++;
            
            UpdateHotResourceStatus(name, info);
        }

        /**
         * @brief Record resource binding event
         */
        void RecordBinding(const std::string& name, ShaderResourceType type, u32 resourceID = 0)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto& info = GetOrCreateResourceInfo(name, type, resourceID);
            info.TotalBindings++;
            info.LastAccess = std::chrono::steady_clock::now();
            
            ChangeResourceState(name, info, ResourceState::Bound);
            
            m_CurrentFrameStats.TotalResourceBindings++;
        }

        /**
         * @brief Record resource update event
         */
        void RecordUpdate(const std::string& name, ShaderResourceType type, sizet dataSize = 0, u32 resourceID = 0)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto& info = GetOrCreateResourceInfo(name, type, resourceID);
            info.TotalUpdates++;
            info.LastUpdate = std::chrono::steady_clock::now();
            info.LastAccess = info.LastUpdate;
            
            if (dataSize > 0)
            {
                info.MemoryUsage = dataSize;
            }
            
            ChangeResourceState(name, info, ResourceState::Updated);
            
            m_CurrentFrameStats.TotalResourceUpdates++;
        }

        /**
         * @brief Record resource state change
         */
        void RecordStateChange(const std::string& name, ResourceState newState)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto it = m_ResourceInfo.find(name);
            if (it != m_ResourceInfo.end())
            {
                ChangeResourceState(name, it->second, newState);
            }
        }

        /**
         * @brief Mark resource as dirty
         */
        void MarkDirty(const std::string& name)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto it = m_ResourceInfo.find(name);
            if (it != m_ResourceInfo.end())
            {
                it->second.IsDirty = true;
                ChangeResourceState(name, it->second, ResourceState::Dirty);
            }
        }

        /**
         * @brief Mark resource as clean
         */
        void MarkClean(const std::string& name)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto it = m_ResourceInfo.find(name);
            if (it != m_ResourceInfo.end())
            {
                it->second.IsDirty = false;
            }
        }

        /**
         * @brief Get resource access information
         */
        ResourceAccessInfo GetResourceInfo(const std::string& name) const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto it = m_ResourceInfo.find(name);
            if (it != m_ResourceInfo.end())
            {
                return it->second;
            }
            
            return ResourceAccessInfo{};
        }

        /**
         * @brief Check if resource is considered "hot" (frequently accessed)
         */
        bool IsHotResource(const std::string& name) const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_HotResources.find(name) != m_HotResources.end();
        }

        /**
         * @brief Get list of all hot resources
         */
        std::vector<std::string> GetHotResources() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return std::vector<std::string>(m_HotResources.begin(), m_HotResources.end());
        }

        /**
         * @brief Get current frame statistics
         */
        FrameStatistics GetCurrentFrameStats() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_CurrentFrameStats;
        }

        /**
         * @brief Get previous frame statistics
         */
        FrameStatistics GetLastFrameStats() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_LastFrameStats;
        }

        /**
         * @brief Advance to next frame (call at beginning of each frame)
         */
        void NextFrame()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            // Finalize current frame statistics
            FinalizeFrameStats();
            
            // Move current stats to last frame
            m_LastFrameStats = m_CurrentFrameStats;
            
            // Reset current frame stats
            m_CurrentFrame++;
            m_CurrentFrameStats = FrameStatistics{};
            m_CurrentFrameStats.FrameNumber = m_CurrentFrame;
            
            // Update hot resource status (decay)
            UpdateHotResourceDecay();
        }

        /**
         * @brief Get all tracked resources
         */
        std::vector<ResourceAccessInfo> GetAllResources() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            std::vector<ResourceAccessInfo> result;
            result.reserve(m_ResourceInfo.size());
            
            for (const auto& [name, info] : m_ResourceInfo)
            {
                result.push_back(info);
            }
            
            return result;
        }

        /**
         * @brief Clear all tracking data
         */
        void Clear()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            m_ResourceInfo.clear();
            m_HotResources.clear();
            m_CurrentFrameStats = FrameStatistics{};
            m_LastFrameStats = FrameStatistics{};
            m_CurrentFrame = 0;
            
            m_TotalResourcesTracked = 0;
            m_TotalAccessesRecorded = 0;
            m_TotalStateChangesRecorded = 0;
        }

        /**
         * @brief Get global statistics
         */
        struct GlobalStatistics
        {
            u64 TotalResourcesTracked = 0;
            u64 TotalAccessesRecorded = 0;
            u64 TotalStateChangesRecorded = 0;
            u32 CurrentHotResourceCount = 0;
            u32 CurrentFrame = 0;
        };

        GlobalStatistics GetGlobalStatistics() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            GlobalStatistics stats;
            stats.TotalResourcesTracked = m_TotalResourcesTracked.load();
            stats.TotalAccessesRecorded = m_TotalAccessesRecorded.load();
            stats.TotalStateChangesRecorded = m_TotalStateChangesRecorded.load();
            stats.CurrentHotResourceCount = static_cast<u32>(m_HotResources.size());
            stats.CurrentFrame = m_CurrentFrame;
            
            return stats;
        }

        /**
         * @brief Configure hot resource detection
         */
        void SetHotResourceThreshold(u64 threshold)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_HotResourceThreshold = threshold;
        }

        void SetHotResourceDecayRate(f64 decayRate)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_HotResourceDecayRate = std::clamp(decayRate, 0.0, 1.0);
        }

    private:
        ResourceAccessInfo& GetOrCreateResourceInfo(const std::string& name, ShaderResourceType type, u32 resourceID)
        {
            auto it = m_ResourceInfo.find(name);
            if (it == m_ResourceInfo.end())
            {
                auto [insertIt, success] = m_ResourceInfo.emplace(name, ResourceAccessInfo(name, type, resourceID));
                m_TotalResourcesTracked++;
                return insertIt->second;
            }
            return it->second;
        }

        void ChangeResourceState(const std::string& name, ResourceAccessInfo& info, ResourceState newState)
        {
            if (info.CurrentState != newState)
            {
                info.CurrentState = newState;
                info.StateChanges++;
                m_CurrentFrameStats.TotalStateChanges++;
                m_TotalStateChangesRecorded++;
            }
        }

        void UpdateHotResourceStatus(const std::string& name, const ResourceAccessInfo& info)
        {
            // Simple hot resource detection: if accessed more than threshold in current frame
            bool wasHot = m_HotResources.find(name) != m_HotResources.end();
            bool isHot = info.TotalAccesses >= m_HotResourceThreshold;
            
            if (isHot && !wasHot)
            {
                m_HotResources.insert(name);
            }
            else if (!isHot && wasHot)
            {
                m_HotResources.erase(name);
            }
        }

        void UpdateHotResourceDecay()
        {
            // Apply decay to hot resource status
            std::vector<std::string> toRemove;
            
            for (const std::string& hotResource : m_HotResources)
            {
                auto it = m_ResourceInfo.find(hotResource);
                if (it != m_ResourceInfo.end())
                {
                    // Simple decay: if not accessed recently, remove from hot list
                    auto timeSinceAccess = std::chrono::steady_clock::now() - it->second.LastAccess;
                    auto secondsSinceAccess = std::chrono::duration_cast<std::chrono::seconds>(timeSinceAccess).count();
                    
                    if (secondsSinceAccess > 5) // 5 seconds without access = not hot anymore
                    {
                        toRemove.push_back(hotResource);
                    }
                }
            }
            
            for (const std::string& resource : toRemove)
            {
                m_HotResources.erase(resource);
            }
        }

        void FinalizeFrameStats()
        {
            // Calculate derived statistics
            if (m_CurrentFrameStats.TotalResourceAccesses > 0)
            {
                m_CurrentFrameStats.AverageAccessesPerResource = 
                    static_cast<f64>(m_CurrentFrameStats.TotalResourceAccesses) / m_ResourceInfo.size();
                    
                m_CurrentFrameStats.StateChangeRate = 
                    static_cast<f64>(m_CurrentFrameStats.TotalStateChanges) / m_CurrentFrameStats.TotalResourceAccesses;
            }
            
            m_CurrentFrameStats.UniqueResourcesAccessed = static_cast<u32>(m_ResourceInfo.size());
            m_CurrentFrameStats.HotResourceCount = static_cast<u32>(m_HotResources.size());
            
            // Calculate total memory usage
            sizet totalMemory = 0;
            for (const auto& [name, info] : m_ResourceInfo)
            {
                totalMemory += info.MemoryUsage;
            }
            m_CurrentFrameStats.TotalMemoryUsed = totalMemory;
        }
    };
}
