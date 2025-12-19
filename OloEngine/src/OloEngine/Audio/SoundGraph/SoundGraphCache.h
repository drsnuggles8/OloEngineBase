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
#include <deque>
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

    //==============================================================================
    /// Cache Configuration Structure

    struct SoundGraphCacheConfig
    {
        // Default configuration constants
        static constexpr sizet s_DefaultMaxCacheSize = 50;
        static constexpr sizet s_DefaultMaxMemoryUsage = 256 * 1024 * 1024; // 256MB
        static constexpr i32 s_DefaultMaintenanceIntervalMinutes = 30;
        static constexpr f32 s_DefaultEvictionThreshold = 0.9f; // Start evicting when 90% full

        sizet m_MaxCacheSize = s_DefaultMaxCacheSize;
        sizet m_MaxMemoryUsage = s_DefaultMaxMemoryUsage;
        bool m_EnableAsyncLoading = true;
        bool m_EnablePersistentCache = true;
        std::string m_CacheDirectory = "cache/soundgraph/";
        i32 m_MaintenanceIntervalMinutes = s_DefaultMaintenanceIntervalMinutes;
        f32 m_EvictionThreshold = s_DefaultEvictionThreshold;
    };

    /// Cache entry for compiled sound graphs
    struct SoundGraphCacheEntry
    {
        std::string m_SourcePath;
        std::string m_CompiledPath;
        sizet m_SourceHash = 0;
        std::chrono::time_point<std::chrono::system_clock> m_LastModified;
        std::chrono::time_point<std::chrono::system_clock> m_LastAccessed;
        Ref<SoundGraph> m_CachedGraph;
        bool m_IsValid = false;
        u32 m_AccessCount = 0;
    };

    /// High-performance cache for compiled sound graphs with LRU eviction
    class SoundGraphCache : public RefCounted
    {
      public:
        // Constructor with explicit parameters (uses config defaults)
        SoundGraphCache(sizet maxCacheSize = 50,
                        sizet maxMemoryUsage = 256 * 1024 * 1024);

        // Constructor with configuration struct
        explicit SoundGraphCache(const SoundGraphCacheConfig& config);

        ~SoundGraphCache();

        /// Cache Management
        bool Has(const std::string& sourcePath) const;
        Ref<SoundGraph> Get(const std::string& sourcePath);
        void Put(const std::string& sourcePath, Ref<SoundGraph> graph, const std::string& compiledPath = "");
        void Remove(const std::string& sourcePath);
        void Clear();

        /// Cache Statistics
        sizet GetSize() const;
        sizet GetMemoryUsage() const;
        f32 GetHitRatio() const;

        /// Configuration
        void SetMaxCacheSize(sizet maxSize);
        void SetMaxMemoryUsage(sizet maxMemory);
        sizet GetMaxCacheSize() const;
        sizet GetMaxMemoryUsage() const;

        /// Cache directory configuration
        void SetCacheDirectory(const std::string& directory);
        std::string GetCacheDirectory() const;

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

        // LRU tracking - most recent at back for O(1) insertion
        std::list<std::string> m_LRUOrder;
        // Iterator map for O(1) LRU removal - maps path to its position in m_LRUOrder
        std::unordered_map<std::string, std::list<std::string>::iterator> m_LRUPositions;

        // Configuration
        sizet m_MaxCacheSize;
        sizet m_MaxMemoryUsage;
        sizet m_CurrentMemoryUsage = 0;
        std::string m_CacheDirectory = "cache/soundgraph/";

        // Statistics
        mutable std::atomic<u64> m_HitCount = 0;
        mutable std::atomic<u64> m_MissCount = 0;

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
    } // namespace CacheUtilities

} // namespace OloEngine::Audio::SoundGraph
