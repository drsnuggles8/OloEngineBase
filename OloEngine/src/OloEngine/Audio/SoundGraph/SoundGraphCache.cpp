#include "OloEnginePCH.h"
#include "SoundGraphCache.h"
#include "SoundGraph.h"
#include "SoundGraphSerializer.h"
#include "SoundGraphPrototype.h" // Must include before SoundGraphAsset.h to complete Prototype type
#include "GraphGeneration.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Task/Task.h"
#include "OloEngine/Task/LowLevelTask.h"
#include "OloEngine/Task/NamedThreads.h"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// SoundGraphCache Implementation

    SoundGraphCache::SoundGraphCache(sizet maxCacheSize, sizet maxMemoryUsage)
        : m_MaxCacheSize(maxCacheSize), m_MaxMemoryUsage(maxMemoryUsage)
    {
        // Task-based loading: no dedicated loader thread needed
        // Each LoadAsync call spawns an independent task via Tasks::Launch()
    }

    SoundGraphCache::SoundGraphCache(const SoundGraphCacheConfig& config)
        : m_MaxCacheSize(config.m_MaxCacheSize), m_MaxMemoryUsage(config.m_MaxMemoryUsage), m_CacheDirectory(config.m_CacheDirectory)
    {
        // Task-based loading: async loading is always available via task system
        // No dedicated loader thread creation needed
    }

    SoundGraphCache::~SoundGraphCache()
    {
        OLO_PROFILE_FUNCTION();

        // Wait for any active load tasks to complete
        // This prevents accessing destroyed cache during in-flight loads
        while (m_ActiveLoadTasks.load(std::memory_order_acquire) > 0)
        {
            // Brief yield to allow tasks to complete
            std::this_thread::yield();
        }

        Clear();
    }

    bool SoundGraphCache::Has(const std::string& sourcePath) const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        auto it = m_CacheEntries.find(sourcePath);
        return it != m_CacheEntries.end() && it->second.m_IsValid;
    }

    Ref<SoundGraph> SoundGraphCache::Get(const std::string& sourcePath)
    {
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        auto it = m_CacheEntries.find(sourcePath);
        if (it == m_CacheEntries.end() || !it->second.m_IsValid)
        {
            ++m_MissCount;
            return nullptr;
        }

        // Update statistics and LRU
        ++m_HitCount;
        it->second.m_LastAccessed = std::chrono::system_clock::now();
        it->second.m_AccessCount++;
        UpdateLRU(sourcePath);

        return it->second.m_CachedGraph;
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

        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

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
        entry.m_SourcePath = sourcePath;
        entry.m_CompiledPath = compiledPath;
        entry.m_SourceHash = sourceHash;
        entry.m_LastModified = lastModified;
        entry.m_LastAccessed = std::chrono::system_clock::now();
        entry.m_CachedGraph = graph;
        entry.m_IsValid = true;
        entry.m_AccessCount = 1;

        // Remove existing entry if it exists
        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            m_CurrentMemoryUsage -= CalculateGraphMemoryUsage(it->second.m_CachedGraph);
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
        OLO_PROFILE_FUNCTION();

        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            m_CurrentMemoryUsage -= CalculateGraphMemoryUsage(it->second.m_CachedGraph);
            RemoveFromLRU(sourcePath);
            m_CacheEntries.erase(it);
        }
    }

    void SoundGraphCache::Clear()
    {
        OLO_PROFILE_FUNCTION();

        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        m_CacheEntries.clear();
        m_LRUOrder.clear();
        m_CurrentMemoryUsage = 0;
        m_HitCount = 0;
        m_MissCount = 0;
    }

    f32 SoundGraphCache::GetHitRatio() const
    {
        OLO_PROFILE_FUNCTION();

        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
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

        // Remove from iterator map - O(1) operation
        m_LRUPositions.erase(lruPath);

        auto it = m_CacheEntries.find(lruPath);
        if (it != m_CacheEntries.end())
        {
            m_CurrentMemoryUsage -= CalculateGraphMemoryUsage(it->second.m_CachedGraph);
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
            TDynamicUniqueLock<FMutex> Lock(m_Mutex);
            for (auto& [path, entry] : m_CacheEntries)
            {
                pathsToCheck.emplace_back(path, entry.m_LastModified);
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
            TDynamicUniqueLock<FMutex> Lock(m_Mutex);
            for (const auto& [path, entry] : m_CacheEntries)
            {
                if (entry.m_LastAccessed < threshold && entry.m_AccessCount < 5)
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
        OLO_PROFILE_FUNCTION();
        // Copy the cached timestamp while holding the mutex to avoid race conditions
        std::optional<std::chrono::time_point<std::chrono::system_clock>> cachedModTime;
        {
            TDynamicUniqueLock<FMutex> Lock(m_Mutex);
            auto it = m_CacheEntries.find(sourcePath);
            if (it == m_CacheEntries.end())
                return true;

            cachedModTime = it->second.m_LastModified;
        }

        // Perform filesystem access outside the lock
        auto currentModTime = GetFileModificationTime(sourcePath);
        return currentModTime > cachedModTime.value();
    }

    void SoundGraphCache::InvalidateByPath(const std::string& sourcePath)
    {
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            it->second.m_IsValid = false;
            OLO_CORE_TRACE("SoundGraphCache: Invalidated entry '{}'", sourcePath);
        }
    }

    void SoundGraphCache::InvalidateByDirectory(const std::string& directoryPath)
    {
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        std::vector<std::string> pathsToInvalidate;

        // Use filesystem::path for robust directory matching
        std::filesystem::path targetDir(directoryPath);
        targetDir = targetDir.lexically_normal();

        for (auto& [path, entry] : m_CacheEntries)
        {
            std::filesystem::path entryPath(path);
            entryPath = entryPath.lexically_normal();

            // Check if entryPath is within targetDir by comparing parent paths
            auto [dirEnd, entryEnd] = std::mismatch(targetDir.begin(), targetDir.end(),
                                                    entryPath.begin(), entryPath.end());

            // Match if we consumed all of targetDir and entryPath has same or more components
            if (dirEnd == targetDir.end())
            {
                pathsToInvalidate.push_back(path);
            }
        }

        for (const std::string& path : pathsToInvalidate)
        {
            m_CacheEntries[path].m_IsValid = false;
        }

        OLO_CORE_INFO("SoundGraphCache: Invalidated {} entries in directory '{}'",
                      pathsToInvalidate.size(), directoryPath);
    }

    void SoundGraphCache::LoadAsync(const std::string& sourcePath, LoadCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        // Increment active task counter before launching
        m_ActiveLoadTasks.fetch_add(1, std::memory_order_relaxed);

        // Capture 'this' and necessary data for the task
        // Using a weak reference pattern would be ideal, but for now we rely on
        // the destructor waiting for active tasks to complete
        Tasks::Launch(
            "SoundGraphLoad",
            [this, path = sourcePath, cb = std::move(callback)]()
            {
                LoadGraphInternal(path, cb);
                m_ActiveLoadTasks.fetch_sub(1, std::memory_order_release);
            },
            LowLevelTasks::ETaskPriority::BackgroundNormal);
    }

    void SoundGraphCache::PreloadGraphs(const std::vector<std::string>& sourcePaths)
    {
        OLO_PROFILE_FUNCTION();
        for (const std::string& path : sourcePaths)
        {
            LoadAsync(path, [](const std::string& path, Ref<SoundGraph> graph)
                      {
                if (graph)
                {
                    OLO_CORE_TRACE("SoundGraphCache: Preloaded graph '{}'", path);
                }
                else
                {
                    OLO_CORE_WARN("SoundGraphCache: Failed to preload graph '{}'", path);
                } });
        }
    }

    std::vector<std::string> SoundGraphCache::GetCachedPaths() const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        std::vector<std::string> paths;
        paths.reserve(m_CacheEntries.size());

        for (const auto& [path, entry] : m_CacheEntries)
        {
            if (entry.m_IsValid)
            {
                paths.push_back(path);
            }
        }

        std::sort(paths.begin(), paths.end());
        return paths;
    }

    std::optional<SoundGraphCacheEntry> SoundGraphCache::GetCacheEntry(const std::string& sourcePath) const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        auto it = m_CacheEntries.find(sourcePath);
        if (it != m_CacheEntries.end())
        {
            return it->second; // Return a copy
        }
        return std::nullopt;
    }

    void SoundGraphCache::LogStatistics() const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);

        OLO_CORE_INFO("SoundGraphCache Statistics:");
        OLO_CORE_INFO("  Entries: {}/{}", m_CacheEntries.size(), m_MaxCacheSize);
        OLO_CORE_INFO("  Memory Usage: {:.2f}/{:.2f} MB",
                      m_CurrentMemoryUsage / (1024.0f * 1024.0f),
                      m_MaxMemoryUsage / (1024.0f * 1024.0f));

        // Compute hit ratio directly within the lock to avoid deadlock and ensure consistency
        u64 totalAccesses = m_HitCount + m_MissCount;
        f32 hitRatio = totalAccesses > 0 ? static_cast<f32>(m_HitCount) / static_cast<f32>(totalAccesses) : 0.0f;
        OLO_CORE_INFO("  Hit Ratio: {:.1f}% ({}/{} requests)",
                      hitRatio * 100.0f, m_HitCount.load(), totalAccesses);
    }

    bool SoundGraphCache::SaveCacheMetadata(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();
        // TODO: Implementation would serialize cache metadata to JSON/binary format
        // This is a placeholder for persistent cache functionality
        OLO_CORE_INFO("SoundGraphCache: Saving cache metadata to '{}'", filePath);
        return true;
    }

    bool SoundGraphCache::LoadCacheMetadata(const std::string& filePath)
    {
        // TODO: Implementation would deserialize cache metadata from JSON/binary format
        // This is a placeholder for persistent cache functionality
        OLO_CORE_INFO("SoundGraphCache: Loading cache metadata from '{}'", filePath);
        return true;
    }

    //==============================================================================
    /// Private Helper Methods

    void SoundGraphCache::UpdateLRU(const std::string& sourcePath)
    {
        OLO_PROFILE_FUNCTION();
        // Remove from current position if exists - O(1) via iterator map
        RemoveFromLRU(sourcePath);

        // Add to back (most recently used) - O(1) operation
        m_LRUOrder.push_back(sourcePath);

        // Store iterator for O(1) future removal
        auto it = m_LRUOrder.end();
        --it; // Point to the element we just added
        m_LRUPositions[sourcePath] = it;
    }

    void SoundGraphCache::RemoveFromLRU(const std::string& sourcePath)
    {
        OLO_PROFILE_FUNCTION();
        // O(1) lookup in iterator map
        auto posIt = m_LRUPositions.find(sourcePath);
        if (posIt != m_LRUPositions.end())
        {
            // O(1) erase from list using stored iterator
            m_LRUOrder.erase(posIt->second);
            // O(1) erase from iterator map
            m_LRUPositions.erase(posIt);
        }
    }

    sizet SoundGraphCache::CalculateGraphMemoryUsage(const Ref<SoundGraph>& graph) const
    {
        OLO_PROFILE_FUNCTION();
        if (!graph)
            return 0;

        sizet totalMemory = 0;

        // Base object size (approximate)
        totalMemory += sizeof(SoundGraph);

        // NodeProcessor base class data structures
        totalMemory += sizeof(NodeProcessor);

        // Calculate memory for all nodes in the graph
        for (const auto& node : graph->m_Nodes)
        {
            if (node)
            {
                // Base node memory
                totalMemory += sizeof(NodeProcessor);

                // NodeProcessor maps and vectors (approximate overhead)
                totalMemory += 256; // Estimated overhead for maps/vectors per node

                // TODO: Check if this is a WavePlayer with audio data
                // WavePlayer nodes can consume significant memory for audio samples
                // AudioData.samples is std::vector<f32> with interleaved audio
                // Estimate: typical audio file might be 1-5MB of samples
                // We'll use a conservative estimate since we can't easily inspect the actual AudioData
                totalMemory += 2 * 1024 * 1024; // 2MB per node (conservative estimate for potential audio data)
            }
        }

        // SoundGraph-specific data structures

        // Nodes vector overhead
        totalMemory += graph->m_Nodes.capacity() * sizeof(Scope<NodeProcessor>);

        // WavePlayers vector (raw pointers, minimal overhead)
        totalMemory += graph->m_WavePlayers.capacity() * sizeof(NodeProcessor*);

        // EndpointInputStreams map (changed from vector to unordered_map for O(1) lookups)
        totalMemory += graph->m_EndpointInputStreams.size() * (sizeof(Identifier) + sizeof(Scope<StreamWriter>));
        totalMemory += graph->m_EndpointInputStreams.bucket_count() * sizeof(void*); // Hash table overhead
        for (const auto& [id, endpoint] : graph->m_EndpointInputStreams)
        {
            if (endpoint)
                totalMemory += sizeof(StreamWriter) + 64; // StreamWriter + estimated choc::value overhead
        }

        // InterpolatedValue map
        totalMemory += graph->m_InterpInputs.size() * (sizeof(Identifier) + sizeof(SoundGraph::InterpolatedValue));
        totalMemory += graph->m_InterpInputs.bucket_count() * sizeof(void*); // Hash table overhead

        // LocalVariables map (changed from vector to unordered_map for O(1) lookups)
        totalMemory += graph->m_LocalVariables.size() * (sizeof(Identifier) + sizeof(Scope<StreamWriter>));
        totalMemory += graph->m_LocalVariables.bucket_count() * sizeof(void*); // Hash table overhead
        for (const auto& [id, localVar] : graph->m_LocalVariables)
        {
            if (localVar)
                totalMemory += sizeof(StreamWriter) + 64; // StreamWriter + estimated overhead
        }

        // Output channel vectors
        totalMemory += graph->m_OutputChannelIDs.capacity() * sizeof(Identifier);
        totalMemory += graph->m_OutChannels.capacity() * sizeof(float);

        // EndpointOutputStreams (NodeProcessor)
        totalMemory += sizeof(NodeProcessor) + 256; // Base size + estimated overhead

        // Thread-safe FIFO queues (choc::fifo::SingleReaderSingleWriterFIFO)
        // These are typically allocated with fixed sizes
        // Note: Cannot access private OutgoingEvent/OutgoingMessage structs directly
        // Using estimated sizes based on typical event/message structure
        totalMemory += 1024 * 64; // Estimated FIFO capacity * estimated event size
        totalMemory += 1024 * 32; // Estimated FIFO capacity * estimated message size
        // choc::value::ValueView objects don't own data, minimal memory overhead
        // String storage for debug names, identifiers, etc.
        totalMemory += 1024; // Estimated string storage overhead

        return totalMemory;
    }

    sizet SoundGraphCache::GetFileSize(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();
        std::error_code ec;
        auto size = std::filesystem::file_size(filePath, ec);
        return ec ? 0 : static_cast<sizet>(size);
    }

    std::chrono::time_point<std::chrono::system_clock> SoundGraphCache::GetFileModificationTime(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(filePath, ec);
        if (ec)
            return std::chrono::system_clock::now();

        // Convert file_time_type to system_clock using C++20 clock_cast for accurate conversion
        // This handles differences between filesystem clock and system clock correctly
        auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
        return sctp;
    }

    sizet SoundGraphCache::HashFile(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();
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
            fileContent.resize(static_cast<sizet>(fileSize));
            file.read(&fileContent[0], fileSize);
        }

        // Use standard library hash function - no overflow issues
        return hasher(fileContent);
    }

    void SoundGraphCache::LoadGraphInternal(const std::string& sourcePath, LoadCallback callback)
    {
        OLO_PROFILE_FUNCTION();

        // Load and compile the sound graph
        Ref<SoundGraph> graph = nullptr;

        try
        {
            // Step 1: Deserialize the SoundGraphAsset from file
            OloEngine::SoundGraphAsset asset;
            if (!SoundGraphSerializer::Deserialize(asset, sourcePath))
            {
                OLO_CORE_ERROR("SoundGraphCache::LoadGraphInternal - Failed to deserialize SoundGraphAsset from '{}'", sourcePath);
                if (callback)
                {
                    // Invoke callback on game thread for thread safety
                    Tasks::EnqueueGameThreadTask([cb = callback, path = sourcePath]()
                                                 { cb(path, nullptr); }, "SoundGraphLoadCallback");
                }
                return;
            }

            OLO_CORE_INFO("SoundGraphCache: Successfully deserialized SoundGraphAsset '{}'", asset.m_Name);

            // Step 2: Compile the asset to a Prototype
            std::vector<UUID> waveAssetsToLoad;
            GraphGeneratorOptions options;
            options.m_Name = asset.m_Name;
            options.m_NumInChannels = 2;                         // Stereo input
            options.m_NumOutChannels = 2;                        // Stereo output
            options.m_GraphPrototype = Ref<Prototype>::Create(); // Create empty prototype for population

            // If the asset already has a compiled prototype, use it; otherwise compile from scratch
            Ref<Prototype> prototype;
            if (asset.m_CompiledPrototype)
            {
                OLO_CORE_INFO("SoundGraphCache: Using pre-compiled prototype for '{}'", asset.m_Name);
                prototype = asset.m_CompiledPrototype;
            }
            else
            {
                OLO_CORE_INFO("SoundGraphCache: Compiling prototype from asset data for '{}'", asset.m_Name);
                prototype = ConstructPrototype(options, waveAssetsToLoad);

                if (!prototype)
                {
                    OLO_CORE_ERROR("SoundGraphCache::LoadGraphInternal - Failed to construct prototype from asset '{}'", sourcePath);
                    if (callback)
                    {
                        Tasks::EnqueueGameThreadTask([cb = callback, path = sourcePath]()
                                                     { cb(path, nullptr); }, "SoundGraphLoadCallback");
                    }
                    return;
                }

                // Cache the compiled prototype in the asset for future use
                asset.m_CompiledPrototype = prototype;
            }

            // Step 3: Create an instance of the SoundGraph from the Prototype
            graph = CreateInstance(prototype);

            if (!graph)
            {
                OLO_CORE_ERROR("SoundGraphCache::LoadGraphInternal - Failed to create SoundGraph instance from prototype '{}'", sourcePath);
                if (callback)
                {
                    Tasks::EnqueueGameThreadTask([cb = callback, path = sourcePath]()
                                                 { cb(path, nullptr); }, "SoundGraphLoadCallback");
                }
                return;
            }

            OLO_CORE_INFO("SoundGraphCache: Successfully created SoundGraph instance '{}'", asset.m_Name);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SoundGraphCache::LoadGraphInternal - Exception during graph loading: {}", e.what());
            if (callback)
            {
                Tasks::EnqueueGameThreadTask([cb = callback, path = sourcePath]()
                                             { cb(path, nullptr); }, "SoundGraphLoadCallback");
            }
            return;
        }

        // Generate compiled path for caching with configurable base directory
        // Convert source path to cache path (e.g., "path/file.soundgraph" -> "cache/soundgraph/file.sgc")
        std::filesystem::path sourcePathFs(sourcePath);
        std::string compiledPath;
        {
            TUniqueLock<FMutex> CacheLock(m_Mutex);
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
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("SoundGraphCache: Unexpected error creating cache directory '{}': {}",
                           cacheDir.string(), e.what());
        }

        // Cache the result if successful
        if (graph)
        {
            Put(sourcePath, graph, compiledPath);
        }

        // Invoke callback on game thread for thread safety
        if (callback)
        {
            Tasks::EnqueueGameThreadTask([cb = callback, path = sourcePath, g = graph]()
                                         { cb(path, g); }, "SoundGraphLoadCallback");
        }
    }

    //==============================================================================
    /// Cache Statistics

    sizet SoundGraphCache::GetSize() const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        return m_CacheEntries.size();
    }

    sizet SoundGraphCache::GetMemoryUsage() const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        return m_CurrentMemoryUsage;
    }

    //==============================================================================
    /// Configuration

    void SoundGraphCache::SetMaxCacheSize(sizet maxSize)
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        m_MaxCacheSize = maxSize;

        // Trigger eviction if current cache exceeds new limit
        while (m_CacheEntries.size() > m_MaxCacheSize && !m_CacheEntries.empty())
        {
            EvictLRU();
        }
    }

    void SoundGraphCache::SetMaxMemoryUsage(sizet maxMemory)
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        m_MaxMemoryUsage = maxMemory;

        // Trigger eviction if current memory usage exceeds new limit
        while (m_CurrentMemoryUsage > m_MaxMemoryUsage && !m_CacheEntries.empty())
        {
            EvictLRU();
        }
    }

    sizet SoundGraphCache::GetMaxCacheSize() const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        return m_MaxCacheSize;
    }

    sizet SoundGraphCache::GetMaxMemoryUsage() const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        return m_MaxMemoryUsage;
    }

    void SoundGraphCache::SetCacheDirectory(const std::string& directory)
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        m_CacheDirectory = directory;

        // Ensure the directory ends with a slash
        if (!m_CacheDirectory.empty() && m_CacheDirectory.back() != '/' && m_CacheDirectory.back() != '\\')
        {
            m_CacheDirectory += '/';
        }
    }

    std::string SoundGraphCache::GetCacheDirectory() const
    {
        OLO_PROFILE_FUNCTION();
        TDynamicUniqueLock<FMutex> Lock(m_Mutex);
        return m_CacheDirectory;
    }

    //==============================================================================
    /// Global Cache Utilities

    namespace CacheUtilities
    {
        static Ref<SoundGraphCache> s_GlobalCache;
        static FMutex s_GlobalCacheMutex;

        Ref<SoundGraphCache> GetGlobalCache()
        {
            TDynamicUniqueLock<FMutex> Lock(s_GlobalCacheMutex);

            if (!s_GlobalCache)
            {
                // Use default configuration
                s_GlobalCache = Ref<SoundGraphCache>::Create(
                    SoundGraphCacheConfig::s_DefaultMaxCacheSize,
                    SoundGraphCacheConfig::s_DefaultMaxMemoryUsage);
            }

            return s_GlobalCache;
        }

        void SetGlobalCache(Ref<SoundGraphCache> cache)
        {
            TDynamicUniqueLock<FMutex> Lock(s_GlobalCacheMutex);
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
            TDynamicUniqueLock<FMutex> Lock(s_GlobalCacheMutex);

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
    } // namespace CacheUtilities

} // namespace OloEngine::Audio::SoundGraph
