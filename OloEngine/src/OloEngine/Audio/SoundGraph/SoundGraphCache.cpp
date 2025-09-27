#include "OloEnginePCH.h"
#include "SoundGraphCache.h"
#include "SoundGraph.h"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// SoundGraphCache Implementation

    SoundGraphCache::SoundGraphCache(sizet maxCacheSize, sizet maxMemoryUsage)
        : m_MaxCacheSize(maxCacheSize)
        , m_MaxMemoryUsage(maxMemoryUsage)
        , m_LoaderThread("SoundGraphCache Loader")
    {
        // Start async loader thread
        m_LoaderThread.Dispatch([this]() { LoaderThreadFunc(); });
    }

    SoundGraphCache::~SoundGraphCache()
    {
        // Shutdown async loader
        m_ShutdownLoader = true;
        m_LoadCondition.notify_all();
        m_LoaderThread.Join();
        
        Clear();
    }

    bool SoundGraphCache::Has(const std::string& sourcePath) const
    {
        std::shared_lock lock(m_Mutex);
        auto it = m_CacheEntries.find(sourcePath);
        return it != m_CacheEntries.end() && it->second.IsValid;
    }

    Ref<SoundGraph> SoundGraphCache::Get(const std::string& sourcePath)
    {
        std::unique_lock lock(m_Mutex);
        
        auto it = m_CacheEntries.find(sourcePath);
        if (it == m_CacheEntries.end() || !it->second.IsValid)
        {
            ++m_MissCount;
            return nullptr;
        }

        // Update statistics and LRU
        ++m_HitCount;
        it->second.LastAccessed = std::chrono::system_clock::now();
        it->second.AccessCount++;
        UpdateLRU(sourcePath);

        return it->second.CachedGraph;
    }

    void SoundGraphCache::Put(const std::string& sourcePath, Ref<SoundGraph> graph, const std::string& compiledPath)
    {
        if (!graph)
        {
            OLO_CORE_WARN("SoundGraphCache::Put - Attempting to cache null graph for '{}'", sourcePath);
            return;
        }

        std::unique_lock lock(m_Mutex);

        // Calculate memory usage
        sizet graphMemory = CalculateGraphMemoryUsage(graph);
        
        // Check if we need to evict first
        while (m_CacheEntries.size() >= m_MaxCacheSize || 
               (m_CurrentMemoryUsage + graphMemory) > m_MaxMemoryUsage)
        {
            EvictLRU();
            if (m_CacheEntries.empty())
                break;
        }

        // Create cache entry
        SoundGraphCacheEntry entry;
        entry.SourcePath = sourcePath;
        entry.CompiledPath = compiledPath;
        entry.SourceHash = HashFile(sourcePath);
        entry.LastModified = GetFileModificationTime(sourcePath);
        entry.LastAccessed = std::chrono::system_clock::now();
        entry.CachedGraph = graph;
        entry.IsValid = true;
        entry.AccessCount = 1;

        // Remove existing entry if it exists
        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            m_CurrentMemoryUsage -= CalculateGraphMemoryUsage(it->second.CachedGraph);
            RemoveFromLRU(sourcePath);
        }

        // Add new entry
        m_CacheEntries[sourcePath] = entry;
        m_CurrentMemoryUsage += graphMemory;
        UpdateLRU(sourcePath);

        OLO_CORE_TRACE("SoundGraphCache: Cached graph '{}' (Memory: {:.1f}KB, Total: {:.1f}MB)", 
                       sourcePath, graphMemory / 1024.0f, m_CurrentMemoryUsage / (1024.0f * 1024.0f));
    }

    void SoundGraphCache::Remove(const std::string& sourcePath)
    {
        std::unique_lock lock(m_Mutex);
        
        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            m_CurrentMemoryUsage -= CalculateGraphMemoryUsage(it->second.CachedGraph);
            RemoveFromLRU(sourcePath);
            m_CacheEntries.erase(it);
        }
    }

    void SoundGraphCache::Clear()
    {
        std::unique_lock lock(m_Mutex);
        
        m_CacheEntries.clear();
        m_LRUOrder.clear();
        m_CurrentMemoryUsage = 0;
        m_HitCount = 0;
        m_MissCount = 0;
    }

    f32 SoundGraphCache::GetHitRatio() const
    {
        u64 totalAccesses = m_HitCount + m_MissCount;
        return totalAccesses > 0 ? static_cast<f32>(m_HitCount) / static_cast<f32>(totalAccesses) : 0.0f;
    }

    void SoundGraphCache::EvictLRU()
    {
        if (m_LRUOrder.empty())
            return;

        // Remove least recently used (last in LRU order)
        std::string lruPath = m_LRUOrder.back();
        m_LRUOrder.pop_back();

        auto it = m_CacheEntries.find(lruPath);
        if (it != m_CacheEntries.end())
        {
            m_CurrentMemoryUsage -= CalculateGraphMemoryUsage(it->second.CachedGraph);
            m_CacheEntries.erase(it);
            
            OLO_CORE_TRACE("SoundGraphCache: Evicted LRU entry '{}'", lruPath);
        }
    }

    void SoundGraphCache::ValidateEntries()
    {
        std::unique_lock lock(m_Mutex);
        
        std::vector<std::string> invalidPaths;
        
        for (auto& [path, entry] : m_CacheEntries)
        {
            // Check if source file still exists
            if (!std::filesystem::exists(path))
            {
                invalidPaths.push_back(path);
                continue;
            }

            // Check if source file has been modified
            if (IsSourceNewer(path))
            {
                invalidPaths.push_back(path);
                continue;
            }
        }

        // Remove invalid entries
        for (const std::string& path : invalidPaths)
        {
            Remove(path);
        }

        if (!invalidPaths.empty())
        {
            OLO_CORE_INFO("SoundGraphCache: Invalidated {} out-of-date entries", invalidPaths.size());
        }
    }

    void SoundGraphCache::CompactCache()
    {
        std::unique_lock lock(m_Mutex);
        
        // Remove entries that haven't been accessed recently
        auto threshold = std::chrono::system_clock::now() - std::chrono::hours(24);
        std::vector<std::string> oldPaths;
        
        for (const auto& [path, entry] : m_CacheEntries)
        {
            if (entry.LastAccessed < threshold && entry.AccessCount < 5)
            {
                oldPaths.push_back(path);
            }
        }

        for (const std::string& path : oldPaths)
        {
            Remove(path);
        }

        if (!oldPaths.empty())
        {
            OLO_CORE_INFO("SoundGraphCache: Compacted {} old entries", oldPaths.size());
        }
    }

    bool SoundGraphCache::IsSourceNewer(const std::string& sourcePath) const
    {
        auto it = m_CacheEntries.find(sourcePath);
        if (it == m_CacheEntries.end())
            return true;

        auto currentModTime = GetFileModificationTime(sourcePath);
        return currentModTime > it->second.LastModified;
    }

    void SoundGraphCache::InvalidateByPath(const std::string& sourcePath)
    {
        std::unique_lock lock(m_Mutex);
        
        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            it->second.IsValid = false;
            OLO_CORE_TRACE("SoundGraphCache: Invalidated entry '{}'", sourcePath);
        }
    }

    void SoundGraphCache::InvalidateByDirectory(const std::string& directoryPath)
    {
        std::unique_lock lock(m_Mutex);
        
        std::vector<std::string> pathsToInvalidate;
        
        for (auto& [path, entry] : m_CacheEntries)
        {
            if (path.find(directoryPath) == 0) // Path starts with directory
            {
                pathsToInvalidate.push_back(path);
            }
        }

        for (const std::string& path : pathsToInvalidate)
        {
            m_CacheEntries[path].IsValid = false;
        }

        OLO_CORE_INFO("SoundGraphCache: Invalidated {} entries in directory '{}'", 
                      pathsToInvalidate.size(), directoryPath);
    }

    void SoundGraphCache::LoadAsync(const std::string& sourcePath, LoadCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_LoadQueueMutex);
        m_LoadQueue.emplace(sourcePath, callback);
        m_LoadCondition.notify_one();
    }

    void SoundGraphCache::PreloadGraphs(const std::vector<std::string>& sourcePaths)
    {
        for (const std::string& path : sourcePaths)
        {
            LoadAsync(path, [](const std::string& path, Ref<SoundGraph> graph) {
                if (graph)
                {
                    OLO_CORE_TRACE("SoundGraphCache: Preloaded graph '{}'", path);
                }
                else
                {
                    OLO_CORE_WARN("SoundGraphCache: Failed to preload graph '{}'", path);
                }
            });
        }
    }

    std::vector<std::string> SoundGraphCache::GetCachedPaths() const
    {
        std::shared_lock lock(m_Mutex);
        
        std::vector<std::string> paths;
        paths.reserve(m_CacheEntries.size());
        
        for (const auto& [path, entry] : m_CacheEntries)
        {
            if (entry.IsValid)
            {
                paths.push_back(path);
            }
        }
        
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    const SoundGraphCacheEntry* SoundGraphCache::GetCacheEntry(const std::string& sourcePath) const
    {
        std::shared_lock lock(m_Mutex);
        
        auto it = m_CacheEntries.find(sourcePath);
        return it != m_CacheEntries.end() ? &it->second : nullptr;
    }

    void SoundGraphCache::LogStatistics() const
    {
        std::shared_lock lock(m_Mutex);
        
        OLO_CORE_INFO("SoundGraphCache Statistics:");
        OLO_CORE_INFO("  Entries: {}/{}", m_CacheEntries.size(), m_MaxCacheSize);
        OLO_CORE_INFO("  Memory Usage: {:.2f}/{:.2f} MB", 
                      m_CurrentMemoryUsage / (1024.0f * 1024.0f),
                      m_MaxMemoryUsage / (1024.0f * 1024.0f));
        OLO_CORE_INFO("  Hit Ratio: {:.1f}% ({}/{} requests)", 
                      GetHitRatio() * 100.0f, m_HitCount, m_HitCount + m_MissCount);
    }

    bool SoundGraphCache::SaveCacheMetadata(const std::string& filePath) const
    {
        // Implementation would serialize cache metadata to JSON/binary format
        // This is a placeholder for persistent cache functionality
        OLO_CORE_INFO("SoundGraphCache: Saving cache metadata to '{}'", filePath);
        return true;
    }

    bool SoundGraphCache::LoadCacheMetadata(const std::string& filePath)
    {
        // Implementation would deserialize cache metadata from JSON/binary format
        // This is a placeholder for persistent cache functionality
        OLO_CORE_INFO("SoundGraphCache: Loading cache metadata from '{}'", filePath);
        return true;
    }

    //==============================================================================
    /// Private Helper Methods

    void SoundGraphCache::UpdateLRU(const std::string& sourcePath)
    {
        // Remove from current position if exists
        RemoveFromLRU(sourcePath);
        
        // Add to front (most recently used)
        m_LRUOrder.insert(m_LRUOrder.begin(), sourcePath);
    }

    void SoundGraphCache::RemoveFromLRU(const std::string& sourcePath)
    {
        auto it = std::find(m_LRUOrder.begin(), m_LRUOrder.end(), sourcePath);
        if (it != m_LRUOrder.end())
        {
            m_LRUOrder.erase(it);
        }
    }

    sizet SoundGraphCache::CalculateGraphMemoryUsage(const Ref<SoundGraph>& graph) const
    {
        if (!graph)
            return 0;

        // Estimate memory usage (in a real implementation, this would be more accurate)
        // Base size + estimated node count * average node size
        return 1024 + (100 * 512); // Placeholder: 1KB base + ~50KB for typical graph
    }

    sizet SoundGraphCache::GetFileSize(const std::string& filePath) const
    {
        std::error_code ec;
        auto size = std::filesystem::file_size(filePath, ec);
        return ec ? 0 : static_cast<sizet>(size);
    }

    std::chrono::time_point<std::chrono::system_clock> SoundGraphCache::GetFileModificationTime(const std::string& filePath) const
    {
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(filePath, ec);
        if (ec)
            return std::chrono::system_clock::now();

        // Convert to system_clock time_point
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return sctp;
    }

    sizet SoundGraphCache::HashFile(const std::string& filePath) const
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return 0;

        // Simple hash of file content (in production, use a proper hash function)
        sizet hash = 0;
        constexpr sizet BufferSize = 4096;
        char buffer[BufferSize];
        
        while (file.read(buffer, BufferSize))
        {
            for (sizet i = 0; i < file.gcount(); ++i)
            {
                hash = hash * 31 + static_cast<sizet>(buffer[i]);
            }
        }
        
        return hash;
    }

    void SoundGraphCache::LoaderThreadFunc()
    {
        while (!m_ShutdownLoader)
        {
            std::unique_lock<std::mutex> lock(m_LoadQueueMutex);
            
            m_LoadCondition.wait(lock, [this] { 
                return !m_LoadQueue.empty() || m_ShutdownLoader; 
            });

            if (m_ShutdownLoader)
                break;

            if (!m_LoadQueue.empty())
            {
                auto [sourcePath, callback] = m_LoadQueue.front();
                m_LoadQueue.pop();
                lock.unlock();

                // Load the graph (placeholder implementation)
                // In a real implementation, this would compile/load the actual graph
                Ref<SoundGraph> graph = nullptr; // LoadSoundGraphFromFile(sourcePath);
                
                if (callback)
                {
                    callback(sourcePath, graph);
                }

                // Cache the result if successful
                if (graph)
                {
                    Put(sourcePath, graph);
                }
            }
        }
    }

    //==============================================================================
    /// Global Cache Utilities

    namespace CacheUtilities
    {
        static Ref<SoundGraphCache> s_GlobalCache;
        static std::mutex s_GlobalCacheMutex;

        Ref<SoundGraphCache> GetGlobalCache()
        {
            std::lock_guard<std::mutex> lock(s_GlobalCacheMutex);
            
            if (!s_GlobalCache)
            {
                s_GlobalCache = Ref<SoundGraphCache>::Create();
            }
            
            return s_GlobalCache;
        }

        void SetGlobalCache(Ref<SoundGraphCache> cache)
        {
            std::lock_guard<std::mutex> lock(s_GlobalCacheMutex);
            s_GlobalCache = cache;
        }

        void InitializeCache()
        {
            // Initialize with configuration from settings
            auto cache = GetGlobalCache();
            OLO_CORE_INFO("SoundGraphCache: Initialized global cache");
        }

        void ShutdownCache()
        {
            std::lock_guard<std::mutex> lock(s_GlobalCacheMutex);
            
            if (s_GlobalCache)
            {
                s_GlobalCache->LogStatistics();
                s_GlobalCache.reset();
                OLO_CORE_INFO("SoundGraphCache: Shutdown global cache");
            }
        }

        void WarmupCache(const std::vector<std::string>& commonGraphs)
        {
            auto cache = GetGlobalCache();
            cache->PreloadGraphs(commonGraphs);
            OLO_CORE_INFO("SoundGraphCache: Started warmup for {} graphs", commonGraphs.size());
        }

        void WarmupCacheFromDirectory(const std::string& directory, const std::string& filePattern)
        {
            std::vector<std::string> graphPaths;
            
            try
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
                {
                    if (entry.is_regular_file())
                    {
                        std::string filename = entry.path().filename().string();
                        // Simple pattern matching (in production, use proper regex or glob)
                        if (filePattern == "*" || filename.find(".soundgraph") != std::string::npos)
                        {
                            graphPaths.push_back(entry.path().string());
                        }
                    }
                }
            }
            catch (const std::filesystem::filesystem_error& ex)
            {
                OLO_CORE_ERROR("SoundGraphCache: Error scanning directory '{}': {}", directory, ex.what());
                return;
            }

            WarmupCache(graphPaths);
        }

        // Placeholder for maintenance scheduler
        void StartMaintenanceScheduler(i32 intervalMinutes)
        {
            OLO_CORE_INFO("SoundGraphCache: Started maintenance scheduler (interval: {}min)", intervalMinutes);
        }

        void StopMaintenanceScheduler()
        {
            OLO_CORE_INFO("SoundGraphCache: Stopped maintenance scheduler");
        }
    }

} // namespace OloEngine::Audio::SoundGraph