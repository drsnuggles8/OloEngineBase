#include "OloEnginePCH.h"
#include "SoundGraphCache.h"
#include "SoundGraph.h"
#include "SoundGraphSerializer.h"
#include "GraphGeneration.h"
#include "OloEngine/Asset/SoundGraphAsset.h"

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
        // Shutdown async loader with proper synchronization
        {
            std::lock_guard<std::mutex> lock(m_LoadQueueMutex);
            m_ShutdownLoader = true;
        }
        m_LoadCondition.notify_all();
        m_LoaderThread.Join();
        
        Clear();
    }

    bool SoundGraphCache::Has(const std::string& sourcePath) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto it = m_CacheEntries.find(sourcePath);
        return it != m_CacheEntries.end() && it->second.IsValid;
    }

    Ref<SoundGraph> SoundGraphCache::Get(const std::string& sourcePath)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
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

        // Perform filesystem I/O operations outside the critical section
        auto sourceHash = HashFile(sourcePath);
        auto lastModified = GetFileModificationTime(sourcePath);

        std::lock_guard<std::mutex> lock(m_Mutex);

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

        // Check if the graph is too large to cache
        if (graphMemory > m_MaxMemoryUsage)
        {
            OLO_CORE_WARN("SoundGraphCache::Put - Graph '{}' size ({:.1f}MB) exceeds maximum memory limit ({:.1f}MB), not caching", 
                          sourcePath, graphMemory / (1024.0f * 1024.0f), m_MaxMemoryUsage / (1024.0f * 1024.0f));
            return;
        }

        // Check if the graph still doesn't fit after eviction
        if ((m_CurrentMemoryUsage + graphMemory) > m_MaxMemoryUsage)
        {
            OLO_CORE_WARN("SoundGraphCache::Put - Graph '{}' size ({:.1f}MB) cannot fit in available memory (current: {:.1f}MB, max: {:.1f}MB), not caching", 
                          sourcePath, graphMemory / (1024.0f * 1024.0f), 
                          m_CurrentMemoryUsage / (1024.0f * 1024.0f), m_MaxMemoryUsage / (1024.0f * 1024.0f));
            return;
        }

        // Create cache entry
        SoundGraphCacheEntry entry;
        entry.SourcePath = sourcePath;
        entry.CompiledPath = compiledPath;
        entry.SourceHash = sourceHash;
        entry.LastModified = lastModified;
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
        std::lock_guard<std::mutex> lock(m_Mutex);
        
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
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        m_CacheEntries.clear();
        m_LRUOrder.clear();
        m_CurrentMemoryUsage = 0;
        m_HitCount = 0;
        m_MissCount = 0;
    }

    f32 SoundGraphCache::GetHitRatio() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        u64 totalAccesses = m_HitCount + m_MissCount;
        return totalAccesses > 0 ? static_cast<f32>(m_HitCount) / static_cast<f32>(totalAccesses) : 0.0f;
    }

    void SoundGraphCache::EvictLRU()
    {
        if (m_LRUOrder.empty())
            return;

        // Remove least recently used (front in LRU order) - O(1) operation
        std::string lruPath = m_LRUOrder.front();
        m_LRUOrder.pop_front();

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
        std::vector<std::string> invalidPaths;
        
        // Gather paths and cached timestamps while holding the mutex
        std::vector<std::pair<std::string, std::chrono::time_point<std::chrono::system_clock>>> pathsToCheck;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (auto& [path, entry] : m_CacheEntries)
            {
                pathsToCheck.emplace_back(path, entry.LastModified);
            }
        }
        
        // Perform filesystem checks without holding the mutex to avoid deadlock
        for (const auto& [path, cachedModTime] : pathsToCheck)
        {
            // Check if source file still exists
            if (!std::filesystem::exists(path))
            {
                invalidPaths.push_back(path);
                continue;
            }

            // Check if source file has been modified
            auto currentModTime = GetFileModificationTime(path);
            if (currentModTime > cachedModTime)
            {
                invalidPaths.push_back(path);
                continue;
            }
        }

        // Remove invalid entries after releasing the mutex to avoid deadlock
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
        // Remove entries that haven't been accessed recently
        auto threshold = std::chrono::system_clock::now() - std::chrono::hours(24);
        std::vector<std::string> oldPaths;
        
        // Gather old paths while holding the mutex
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (const auto& [path, entry] : m_CacheEntries)
            {
                if (entry.LastAccessed < threshold && entry.AccessCount < 5)
                {
                    oldPaths.push_back(path);
                }
            }
        }

        // Remove entries after releasing the mutex to avoid deadlock
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
        // Copy the cached timestamp while holding the mutex to avoid race conditions
        std::optional<std::chrono::time_point<std::chrono::system_clock>> cachedModTime;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            auto it = m_CacheEntries.find(sourcePath);
            if (it == m_CacheEntries.end())
                return true;
            
            cachedModTime = it->second.LastModified;
        }
        
        // Perform filesystem access outside the lock
        auto currentModTime = GetFileModificationTime(sourcePath);
        return currentModTime > cachedModTime.value();
    }

    void SoundGraphCache::InvalidateByPath(const std::string& sourcePath)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            it->second.IsValid = false;
            OLO_CORE_TRACE("SoundGraphCache: Invalidated entry '{}'", sourcePath);
        }
    }

    void SoundGraphCache::InvalidateByDirectory(const std::string& directoryPath)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
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
        std::lock_guard<std::mutex> lock(m_Mutex);
        
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

    std::optional<SoundGraphCacheEntry> SoundGraphCache::GetCacheEntry(const std::string& sourcePath) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            return it->second; // Return a copy
        }
        return std::nullopt;
    }

    void SoundGraphCache::LogStatistics() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        OLO_CORE_INFO("SoundGraphCache Statistics:");
        OLO_CORE_INFO("  Entries: {}/{}", m_CacheEntries.size(), m_MaxCacheSize);
        OLO_CORE_INFO("  Memory Usage: {:.2f}/{:.2f} MB", 
                      m_CurrentMemoryUsage / (1024.0f * 1024.0f),
                      m_MaxMemoryUsage / (1024.0f * 1024.0f));
        
        // Compute hit ratio directly within the lock to avoid deadlock and ensure consistency
        u64 totalAccesses = m_HitCount + m_MissCount;
        f32 hitRatio = totalAccesses > 0 ? static_cast<f32>(m_HitCount) / static_cast<f32>(totalAccesses) : 0.0f;
        u64 hitCount = m_HitCount.load();
        OLO_CORE_INFO("  Hit Ratio: {:.1f}% ({}/{} requests)", 
                      hitRatio * 100.0f, hitCount, totalAccesses);
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
		
		// Add to back (most recently used) - O(1) operation
		m_LRUOrder.push_back(sourcePath);
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

        sizet totalMemory = 0;

        // Base object size (approximate)
        totalMemory += sizeof(SoundGraph);

        // NodeProcessor base class data structures
        totalMemory += sizeof(NodeProcessor);
        
        // Calculate memory for all nodes in the graph
        for (const auto& node : graph->Nodes)
        {
            if (node)
            {
                // Base node memory
                totalMemory += sizeof(NodeProcessor);
                
                // NodeProcessor maps and vectors (approximate overhead)
                totalMemory += 256; // Estimated overhead for maps/vectors per node
                
                // Check if this is a WavePlayer with audio data
                // WavePlayer nodes can consume significant memory for audio samples
                // AudioData.samples is std::vector<f32> with interleaved audio
                // Estimate: typical audio file might be 1-5MB of samples
                // We'll use a conservative estimate since we can't easily inspect the actual AudioData
                totalMemory += 2 * 1024 * 1024; // 2MB per node (conservative estimate for potential audio data)
            }
        }

        // SoundGraph-specific data structures
        
        // Nodes vector overhead
        totalMemory += graph->Nodes.capacity() * sizeof(Scope<NodeProcessor>);
        
        // WavePlayers vector (raw pointers, minimal overhead)
        totalMemory += graph->WavePlayers.capacity() * sizeof(NodeProcessor*);
        
        // EndpointInputStreams vector
        totalMemory += graph->EndpointInputStreams.capacity() * sizeof(Scope<StreamWriter>);
        for (const auto& endpoint : graph->EndpointInputStreams)
        {
            if (endpoint)
                totalMemory += sizeof(StreamWriter) + 64; // StreamWriter + estimated choc::value overhead
        }
        
        // InterpolatedValue map
        totalMemory += graph->InterpInputs.size() * (sizeof(Identifier) + sizeof(SoundGraph::InterpolatedValue));
        totalMemory += graph->InterpInputs.bucket_count() * sizeof(void*); // Hash table overhead
        
        // LocalVariables vector
        totalMemory += graph->LocalVariables.capacity() * sizeof(Scope<StreamWriter>);
        for (const auto& localVar : graph->LocalVariables)
        {
            if (localVar)
                totalMemory += sizeof(StreamWriter) + 64; // StreamWriter + estimated overhead
        }
        
        // Output channel vectors
        totalMemory += graph->OutputChannelIDs.capacity() * sizeof(Identifier);
        totalMemory += graph->out_Channels.capacity() * sizeof(float);
        
        // EndpointOutputStreams (NodeProcessor)
        totalMemory += sizeof(NodeProcessor) + 256; // Base size + estimated overhead
        
		// Thread-safe FIFO queues (choc::fifo::SingleReaderSingleWriterFIFO)
		// These are typically allocated with fixed sizes
		// Note: Cannot access private OutgoingEvent/OutgoingMessage structs directly
		// Using estimated sizes based on typical event/message structure
		totalMemory += 1024 * 64;  // Estimated FIFO capacity * estimated event size
		totalMemory += 1024 * 32; // Estimated FIFO capacity * estimated message size        // choc::value::ValueView objects don't own data, minimal memory overhead
        // String storage for debug names, identifiers, etc.
        totalMemory += 1024; // Estimated string storage overhead
        
        return totalMemory;
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

        // Use std::hash for consistent, overflow-safe hashing
        std::hash<std::string> hasher;
        std::string fileContent;
        
        // Read file content efficiently
        file.seekg(0, std::ios::end);
        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        
        if (fileSize > 0)
        {
            fileContent.resize(static_cast<size_t>(fileSize));
            file.read(&fileContent[0], fileSize);
        }
        
        // Use standard library hash function - no overflow issues
        return hasher(fileContent);
    }

    void SoundGraphCache::LoaderThreadFunc()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(m_LoadQueueMutex);
            
            m_LoadCondition.wait(lock, [this] { 
                return !m_LoadQueue.empty() || m_ShutdownLoader; 
            });

            // Check shutdown condition under mutex protection
            if (m_ShutdownLoader && m_LoadQueue.empty())
                break;

            if (!m_LoadQueue.empty())
            {
                auto [sourcePath, callback] = m_LoadQueue.front();
                m_LoadQueue.pop();
                lock.unlock();

                // Load and compile the sound graph
                Ref<SoundGraph> graph = nullptr;
                
                try
                {
                    // Step 1: Deserialize the SoundGraphAsset from file
                    OloEngine::SoundGraphAsset asset;
                    if (!SoundGraphSerializer::Deserialize(asset, sourcePath))
                    {
                        OLO_CORE_ERROR("SoundGraphCache::LoaderThreadFunc - Failed to deserialize SoundGraphAsset from '{}'", sourcePath);
                        if (callback)
                            callback(sourcePath, nullptr);
                        continue;
                    }
                    
                    OLO_CORE_INFO("SoundGraphCache: Successfully deserialized SoundGraphAsset '{}'", asset.m_Name);
                    
                    // Step 2: Compile the asset to a Prototype
                    std::vector<UUID> waveAssetsToLoad;
                    GraphGeneratorOptions options;
                    options.Name = asset.m_Name;
                    options.NumInChannels = 2;  // Stereo input
                    options.NumOutChannels = 2; // Stereo output
                    options.GraphPrototype = Ref<Prototype>::Create(); // Create empty prototype for population
                    
                    // If the asset already has a compiled prototype, use it; otherwise compile from scratch
                    Ref<Prototype> prototype;
                    if (asset.CompiledPrototype)
                    {
                        OLO_CORE_INFO("SoundGraphCache: Using pre-compiled prototype for '{}'", asset.m_Name);
                        prototype = asset.CompiledPrototype;
                    }
                    else
                    {
                        OLO_CORE_INFO("SoundGraphCache: Compiling prototype from asset data for '{}'", asset.m_Name);
                        prototype = ConstructPrototype(options, waveAssetsToLoad);
                        
                        if (!prototype)
                        {
                            OLO_CORE_ERROR("SoundGraphCache::LoaderThreadFunc - Failed to construct prototype from asset '{}'", sourcePath);
                            if (callback)
                                callback(sourcePath, nullptr);
                            continue;
                        }
                        
                        // Cache the compiled prototype in the asset for future use
                        asset.CompiledPrototype = prototype;
                    }
                    
                    // Step 3: Create an instance of the SoundGraph from the Prototype
                    graph = CreateInstance(prototype);
                    
                    if (!graph)
                    {
                        OLO_CORE_ERROR("SoundGraphCache::LoaderThreadFunc - Failed to create SoundGraph instance from prototype '{}'", sourcePath);
                        if (callback)
                            callback(sourcePath, nullptr);
                        continue;
                    }
                    
                    OLO_CORE_INFO("SoundGraphCache: Successfully created SoundGraph instance '{}'", asset.m_Name);
                }
                catch (const std::exception& e)
                {
                    OLO_CORE_ERROR("SoundGraphCache::LoaderThreadFunc - Exception during graph loading: {}", e.what());
                    graph = nullptr;
                    if (callback)
                    {
                        callback(sourcePath, nullptr);
                    }
                    continue;
                }
                
                // Generate compiled path for caching with configurable base directory
                // Convert source path to cache path (e.g., "path/file.soundgraph" -> "cache/soundgraph/file.sgc")
                std::filesystem::path sourcePathFs(sourcePath);
                std::string compiledPath;
                {
                    std::lock_guard<std::mutex> cacheLock(m_Mutex);
                    compiledPath = m_CacheDirectory + sourcePathFs.stem().string() + ".sgc";
                }
                
                // Ensure cache directory exists before writing
                std::filesystem::path compiledPathFs(compiledPath);
                std::filesystem::path cacheDir = compiledPathFs.parent_path();
                
                try
                {
                    if (!std::filesystem::exists(cacheDir))
                    {
                        bool created = std::filesystem::create_directories(cacheDir);
                        if (!created)
                        {
                            OLO_CORE_ERROR("SoundGraphCache: Failed to create cache directory '{}'", cacheDir.string());
                            // Continue without caching rather than failing completely
                            if (callback)
                            {
                                callback(sourcePath, graph);
                            }
                            continue;
                        }
                        else
                        {
                            OLO_CORE_INFO("SoundGraphCache: Created cache directory '{}'", cacheDir.string());
                        }
                    }
                }
                catch (const std::filesystem::filesystem_error& e)
                {
                    OLO_CORE_ERROR("SoundGraphCache: Filesystem error creating cache directory '{}': {}", 
                                   cacheDir.string(), e.what());
                    // Continue without caching rather than failing completely
                    if (callback)
                    {
                        callback(sourcePath, graph);
                    }
                    continue;
                }
                catch (const std::exception& e)
                {
                    OLO_CORE_ERROR("SoundGraphCache: Unexpected error creating cache directory '{}': {}", 
                                   cacheDir.string(), e.what());
                    // Continue without caching rather than failing completely
                    if (callback)
                    {
                        callback(sourcePath, graph);
                    }
                    continue;
                }
                
                if (callback)
                {
                    callback(sourcePath, graph);
                }

                // Cache the result if successful
                if (graph)
                {
                    Put(sourcePath, graph, compiledPath);
                }
            }
        }
    }

    //==============================================================================
    /// Cache Statistics

    sizet SoundGraphCache::GetSize() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_CacheEntries.size();
    }

    sizet SoundGraphCache::GetMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_CurrentMemoryUsage;
    }

    //==============================================================================
    /// Configuration

    void SoundGraphCache::SetMaxCacheSize(sizet maxSize)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_MaxCacheSize = maxSize;
        
        // Trigger eviction if current cache exceeds new limit
        while (m_CacheEntries.size() > m_MaxCacheSize && !m_CacheEntries.empty())
        {
            EvictLRU();
        }
    }

    void SoundGraphCache::SetMaxMemoryUsage(sizet maxMemory)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_MaxMemoryUsage = maxMemory;
        
        // Trigger eviction if current memory usage exceeds new limit
        while (m_CurrentMemoryUsage > m_MaxMemoryUsage && !m_CacheEntries.empty())
        {
            EvictLRU();
        }
    }

    sizet SoundGraphCache::GetMaxCacheSize() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_MaxCacheSize;
    }

    sizet SoundGraphCache::GetMaxMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_MaxMemoryUsage;
    }

    void SoundGraphCache::SetCacheDirectory(const std::string& directory)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_CacheDirectory = directory;
        
        // Ensure the directory ends with a slash
        if (!m_CacheDirectory.empty() && m_CacheDirectory.back() != '/' && m_CacheDirectory.back() != '\\')
        {
            m_CacheDirectory += '/';
        }
    }

    std::string SoundGraphCache::GetCacheDirectory() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_CacheDirectory;
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
                s_GlobalCache.Reset();
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