#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/Thread.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <queue>
#include <mutex>
#include <vector>
#include <optional>
#include <utility>

namespace OloEngine::Audio::SoundGraph
{
    class SoundGraph;
    class SoundGraphPrototype;

    /// Cache entry for compiled sound graphs
    struct SoundGraphCacheEntry
    {
        std::string SourcePath;
        std::string CompiledPath;
        sizet SourceHash = 0;
        std::chrono::time_point<std::chrono::system_clock> LastModified;
        std::chrono::time_point<std::chrono::system_clock> LastAccessed;
        Ref<SoundGraph> CachedGraph;
        bool IsValid = false;
        u32 AccessCount = 0;
    };

    /// High-performance cache for compiled sound graphs with LRU eviction
    class SoundGraphCache : public RefCounted
    {
    public:
        SoundGraphCache(sizet maxCacheSize = 50, sizet maxMemoryUsage = 256 * 1024 * 1024); // 256MB default
        ~SoundGraphCache();

        /// Cache Management
        bool Has(const std::string& sourcePath) const;
        Ref<SoundGraph> Get(const std::string& sourcePath);
        void Put(const std::string& sourcePath, Ref<SoundGraph> graph, const std::string& compiledPath = "");
        void Remove(const std::string& sourcePath);
        void Clear();

        /// Cache Statistics
        sizet GetSize() const { return m_CacheEntries.size(); }
        sizet GetMemoryUsage() const { return m_CurrentMemoryUsage; }
        f32 GetHitRatio() const;
        
        /// Configuration
        void SetMaxCacheSize(sizet maxSize) { m_MaxCacheSize = maxSize; }
        void SetMaxMemoryUsage(sizet maxMemory) { m_MaxMemoryUsage = maxMemory; }
        sizet GetMaxCacheSize() const { return m_MaxCacheSize; }
        sizet GetMaxMemoryUsage() const { return m_MaxMemoryUsage; }

        /// Cache Maintenance
        void EvictLRU();
        void ValidateEntries();
        void CompactCache();
        
        /// File System Integration
        bool IsSourceNewer(const std::string& sourcePath) const;
        void InvalidateByPath(const std::string& sourcePath);
        void InvalidateByDirectory(const std::string& directoryPath);
        
        /// Async Loading Support
        using LoadCallback = std::function<void(const std::string&, Ref<SoundGraph>)>;
        void LoadAsync(const std::string& sourcePath, LoadCallback callback);
        void PreloadGraphs(const std::vector<std::string>& sourcePaths);
        
        /// Debugging and Introspection
        std::vector<std::string> GetCachedPaths() const;
        std::optional<SoundGraphCacheEntry> GetCacheEntry(const std::string& sourcePath) const;
        void LogStatistics() const;
        
        /// Serialization for persistent cache
        bool SaveCacheMetadata(const std::string& filePath) const;
        bool LoadCacheMetadata(const std::string& filePath);

    private:
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, SoundGraphCacheEntry> m_CacheEntries;
        
        // LRU tracking
        std::vector<std::string> m_LRUOrder; // Most recent first
        
        // Configuration
        sizet m_MaxCacheSize;
        sizet m_MaxMemoryUsage;
        sizet m_CurrentMemoryUsage = 0;
        
        // Statistics
        mutable u64 m_HitCount = 0;
        mutable u64 m_MissCount = 0;
        
        // Async loading
        Thread m_LoaderThread;
        std::queue<std::pair<std::string, LoadCallback>> m_LoadQueue;
        std::mutex m_LoadQueueMutex;
        std::condition_variable m_LoadCondition;
        std::atomic<bool> m_ShutdownLoader = false;
        
        // Helper methods
        void UpdateLRU(const std::string& sourcePath);
        void RemoveFromLRU(const std::string& sourcePath);
        sizet CalculateGraphMemoryUsage(const Ref<SoundGraph>& graph) const;
        sizet GetFileSize(const std::string& filePath) const;
        std::chrono::time_point<std::chrono::system_clock> GetFileModificationTime(const std::string& filePath) const;
        sizet HashFile(const std::string& filePath) const;
        
        // Async loading thread function
        void LoaderThreadFunc();
        
        // Cache eviction policies
        void EvictBySize();
        void EvictByMemory();
        void EvictByAge();
    };

    /// Global cache utilities
    namespace CacheUtilities
    {
        /// Get or create the global sound graph cache instance
        Ref<SoundGraphCache> GetGlobalCache();
        
        /// Set custom global cache instance
        void SetGlobalCache(Ref<SoundGraphCache> cache);
        
        /// Initialize cache with configuration from settings
        void InitializeCache();
        
        /// Shutdown and cleanup cache
        void ShutdownCache();
        
        /// Cache warming utilities
        void WarmupCache(const std::vector<std::string>& commonGraphs);
        void WarmupCacheFromDirectory(const std::string& directory, const std::string& filePattern = "*.soundgraph");
        
        /// Cache maintenance scheduler
        void StartMaintenanceScheduler(i32 intervalMinutes = 30);
        void StopMaintenanceScheduler();
    }

    //==============================================================================
    /// Cache Configuration Structure
    
    struct SoundGraphCacheConfig
    {
        sizet MaxCacheSize = 50;
        sizet MaxMemoryUsage = 256 * 1024 * 1024; // 256MB
        bool EnableAsyncLoading = true;
        bool EnablePersistentCache = true;
        std::string CacheDirectory = "cache/soundgraph/";
        i32 MaintenanceIntervalMinutes = 30;
        f32 EvictionThreshold = 0.9f; // Start evicting when 90% full
    };

} // namespace OloEngine::Audio::SoundGraph