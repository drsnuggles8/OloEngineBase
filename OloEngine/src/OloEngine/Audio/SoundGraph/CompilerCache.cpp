#include "OloEnginePCH.h"
#include "CompilerCache.h"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// CompilerCache Implementation

	CompilerCache::CompilerCache(const std::string& cacheDirectory)
		: m_CacheDirectory(cacheDirectory)
	{
		// Initialize directory creation with enhanced error handling
		try
		{
			CreateCacheDirectory();
			m_DirectoryInitialized = true;
			OLO_CORE_INFO("CompilerCache: Successfully created/verified cache directory '{}'", m_CacheDirectory);
		}
		catch (const std::filesystem::filesystem_error& ex)
		{
			std::string error = "Filesystem error during directory creation: " + std::string(ex.what());
			m_InitializationErrors += error + "; ";
			OLO_CORE_ERROR("CompilerCache: {}", error);
			m_DirectoryInitialized = false;
			// Continue - cache will work in memory-only mode
		}
		catch (const std::exception& ex)
		{
			std::string error = "Exception during directory creation: " + std::string(ex.what());
			m_InitializationErrors += error + "; ";
			OLO_CORE_ERROR("CompilerCache: {}", error);
			m_DirectoryInitialized = false;
			// Continue - cache will work in memory-only mode
		}
		catch (...)
		{
			std::string error = "Unknown exception during directory creation";
			m_InitializationErrors += error + "; ";
			OLO_CORE_ERROR("CompilerCache: {}", error);
			m_DirectoryInitialized = false;
			// Continue - cache will work in memory-only mode
		}
		
		// Initialize disk cache loading with enhanced error handling
		try
		{
			LoadFromDisk();
			m_DiskCacheLoaded = true;
			OLO_CORE_INFO("CompilerCache: Successfully loaded {} entries from disk", m_CompiledResults.size());
		}
		catch (const std::filesystem::filesystem_error& ex)
		{
			std::string error = "Filesystem error during cache loading: " + std::string(ex.what());
			m_InitializationErrors += error + "; ";
			OLO_CORE_WARN("CompilerCache: {}", error);
			m_DiskCacheLoaded = false;
			// Continue - start with empty cache
		}
		catch (const std::exception& ex)
		{
			std::string error = "Exception during cache loading: " + std::string(ex.what());
			m_InitializationErrors += error + "; ";
			OLO_CORE_WARN("CompilerCache: {}", error);
			m_DiskCacheLoaded = false;
			// Continue - start with empty cache
		}
		catch (...)
		{
			std::string error = "Unknown exception during cache loading";
			m_InitializationErrors += error + "; ";
			OLO_CORE_WARN("CompilerCache: {}", error);
			m_DiskCacheLoaded = false;
			// Continue - start with empty cache
		}
		
		// Log final initialization status
		if (IsFullyInitialized())
		{
			OLO_CORE_INFO("CompilerCache: Fully initialized successfully");
		}
		else
		{
			OLO_CORE_WARN("CompilerCache: Partially initialized - some features may be limited. Errors: {}", 
						  m_InitializationErrors.empty() ? "None" : m_InitializationErrors);
		}
	}
    
    bool CompilerCache::HasCompiled(const std::string& sourcePath, const std::string& compilerVersion) const
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        std::string key = GenerateCacheKey(sourcePath, compilerVersion);
        auto it = m_CompiledResults.find(key);
        
        if (it == m_CompiledResults.end() || !it->second || !it->second->m_IsValid)
        {
            return false;
        }

        // Check if source is newer than compilation
        return !IsSourceNewer(sourcePath, *it->second);
    }

	std::shared_ptr<const CompilationResult> CompilerCache::GetCompiled(const std::string& sourcePath, const std::string& compilerVersion) const
	{
		OLO_PROFILE_FUNCTION();
		
		std::lock_guard<std::mutex> lock(m_Mutex);
		
		std::string key = GenerateCacheKey(sourcePath, compilerVersion);
		auto it = m_CompiledResults.find(key);
		
		if (it == m_CompiledResults.end() || !it->second || !it->second->m_IsValid)
		{
			++m_MissCount;
			return nullptr;
		}

		// Check if source is newer than compilation
		if (IsSourceNewer(sourcePath, *it->second))
		{
			++m_MissCount;
			it->second->m_IsValid = false; // Invalidate outdated entry
			return nullptr;
		}

		++m_HitCount;
		return it->second; // Return shared_ptr for thread-safe access
	}    void CompilerCache::StoreCompiled(const std::string& sourcePath, const CompilationResult& result)
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        std::string key = GenerateCacheKey(sourcePath, result.m_CompilerVersion);
        
        // Check if we need to evict old entries
        if (m_CompiledResults.size() >= m_MaxCacheSize)
        {
            // Simple eviction: remove oldest entry
            auto oldestIt = std::min_element(m_CompiledResults.begin(), m_CompiledResults.end(),
                [](const auto& a, const auto& b) {
                    return a.second && b.second && a.second->m_CompilationTime < b.second->m_CompilationTime;
                });
            
            if (oldestIt != m_CompiledResults.end())
            {
                m_CompiledResults.erase(oldestIt);
            }
        }

        m_CompiledResults[key] = std::make_shared<CompilationResult>(result);
        
        if (m_AutoSave)
        {
            // Save to disk asynchronously in production
            std::string filePath = GetCacheFilePath(sourcePath, result.m_CompilerVersion);
            SerializeResult(result, filePath);
        }

        OLO_CORE_TRACE("CompilerCache: Stored compiled result for '{}' ({}ms compilation)", 
                       sourcePath, result.m_CompilationTimeMs);
    }

    void CompilerCache::InvalidateCompiled(const std::string& sourcePath)
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Invalidate all versions of this source file
        for (auto& [key, resultPtr] : m_CompiledResults)
        {
            if (resultPtr && resultPtr->m_SourcePath == sourcePath)
            {
                resultPtr->m_IsValid = false;
            }
        }
    }

	void CompilerCache::InvalidateCompiled(const std::string& sourcePath, const std::string& compilerVersion)
	{
		OLO_PROFILE_FUNCTION();
		
		std::lock_guard<std::mutex> lock(m_Mutex);
		
		std::string key = GenerateCacheKey(sourcePath, compilerVersion);
		auto it = m_CompiledResults.find(key);
		if (it != m_CompiledResults.end())
		{
			auto& resultPtr = it->second;
			if (resultPtr)
			{
				resultPtr->m_IsValid = false;
			}
		}
	}
    
    void CompilerCache::ClearCache()
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        m_CompiledResults.clear();
        m_HitCount = 0;
        m_MissCount = 0;
        
        // Clear disk cache
        try
        {
            if (std::filesystem::exists(m_CacheDirectory))
            {
                std::filesystem::remove_all(m_CacheDirectory);
                CreateCacheDirectory();
            }
        }
        catch (const std::filesystem::filesystem_error& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Failed to clear disk cache: {}", ex.what());
        }
    }

    bool CompilerCache::IsSourceNewer(const std::string& sourcePath, const CompilationResult& result) const
    {
        auto sourceModTime = GetFileModificationTime(sourcePath);
        return sourceModTime > result.m_CompilationTime;
    }

    std::string CompilerCache::GetCacheFilePath(const std::string& sourcePath, const std::string& compilerVersion) const
    {
        std::string key = GenerateCacheKey(sourcePath, compilerVersion);
        sizet hash = HashString(key);
        return m_CacheDirectory + "/" + std::to_string(hash) + ".compiled";
    }

    bool CompilerCache::LoadFromDisk()
    {
        OLO_PROFILE_FUNCTION();
        
        if (!std::filesystem::exists(m_CacheDirectory))
        {
            return true; // No cache directory is fine
        }

        std::lock_guard<std::mutex> lock(m_Mutex);
        
        try
        {
            sizet loadedCount = 0;
            
            for (const auto& entry : std::filesystem::directory_iterator(m_CacheDirectory))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".compiled")
                {
                    CompilationResult result;
                    if (DeserializeResult(result, entry.path().string()))
                    {
                        std::string key = GenerateCacheKey(result.m_SourcePath, result.m_CompilerVersion);
                        m_CompiledResults[key] = std::make_shared<CompilationResult>(result);
                        ++loadedCount;
                    }
                }
            }
            
            OLO_CORE_INFO("CompilerCache: Loaded {} compiled results from disk", loadedCount);
            return true;
        }
        catch (const std::filesystem::filesystem_error& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Failed to load from disk: {}", ex.what());
            return false;
        }
    }

    bool CompilerCache::SaveToDisk() const
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        try
        {
            CreateCacheDirectory();
            
            sizet savedCount = 0;
            
            for (const auto& [key, resultPtr] : m_CompiledResults)
            {
                if (resultPtr && resultPtr->m_IsValid)
                {
                    std::string filePath = GetCacheFilePath(resultPtr->m_SourcePath, resultPtr->m_CompilerVersion);
                    if (SerializeResult(*resultPtr, filePath))
                    {
                        ++savedCount;
                    }
                }
            }
            
            OLO_CORE_INFO("CompilerCache: Saved {} compiled results to disk", savedCount);
            return true;
        }
        catch (const std::filesystem::filesystem_error& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Filesystem error while saving to disk: {}", ex.what());
            return false;
        }
        catch (const std::exception& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Failed to save to disk: {}", ex.what());
            return false;
        }
    }

    void CompilerCache::ValidateAllEntries()
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        std::vector<std::string> invalidKeys;
        
        for (auto& [key, resultPtr] : m_CompiledResults)
        {
            if (!resultPtr)
            {
                invalidKeys.push_back(key);
                continue;
            }

            // Check if source file exists
            if (!std::filesystem::exists(resultPtr->m_SourcePath))
            {
                invalidKeys.push_back(key);
                continue;
            }

            // Check if source is newer than compilation
            if (IsSourceNewer(resultPtr->m_SourcePath, *resultPtr))
            {
                resultPtr->m_IsValid = false;
            }
        }

        // Remove entries for non-existent source files
        for (const std::string& key : invalidKeys)
        {
            m_CompiledResults.erase(key);
        }

        if (!invalidKeys.empty())
        {
            OLO_CORE_INFO("CompilerCache: Removed {} entries for deleted source files", invalidKeys.size());
        }
    }

    void CompilerCache::CleanupOldEntries(std::chrono::hours maxAge)
    {
        OLO_PROFILE_FUNCTION();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        auto threshold = std::chrono::system_clock::now() - maxAge;
        std::vector<std::string> oldKeys;
        
        for (const auto& [key, resultPtr] : m_CompiledResults)
        {
            if (resultPtr && resultPtr->m_CompilationTime < threshold)
            {
                oldKeys.push_back(key);
            }
        }

        for (const std::string& key : oldKeys)
        {
            m_CompiledResults.erase(key);
        }

        if (!oldKeys.empty())
        {
            OLO_CORE_INFO("CompilerCache: Cleaned up {} old entries", oldKeys.size());
        }
    }

    void CompilerCache::CompactCache()
    {
        OLO_PROFILE_FUNCTION();
        
        ValidateAllEntries();
        CleanupOldEntries();
        
        if (m_AutoSave)
        {
            SaveToDisk();
        }
    }

    sizet CompilerCache::GetTotalDiskUsage() const
    {
        OLO_PROFILE_FUNCTION();

        sizet totalSize = 0;        
        try
        {
            if (std::filesystem::exists(m_CacheDirectory))
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(m_CacheDirectory))
                {
                    if (entry.is_regular_file())
                    {
                        totalSize += entry.file_size();
                    }
                }
            }
        }
        catch (const std::filesystem::filesystem_error&)
        {
            // Ignore errors
        }
        
        return totalSize;
    }

    f32 CompilerCache::GetCacheHitRatio() const
    {
        OLO_PROFILE_FUNCTION();

        u64 totalRequests = m_HitCount + m_MissCount;
        return totalRequests > 0 ? static_cast<f32>(m_HitCount) / static_cast<f32>(totalRequests) : 0.0f;
    }

    void CompilerCache::LogStatistics() const
    {
        OLO_PROFILE_FUNCTION();

        std::lock_guard<std::mutex> lock(m_Mutex);
        
        OLO_CORE_INFO("CompilerCache Statistics:");
        OLO_CORE_INFO("  Entries: {}/{}", m_CompiledResults.size(), m_MaxCacheSize);
        OLO_CORE_INFO("  Disk Usage: {:.2f} MB", GetTotalDiskUsage() / (1024.0f * 1024.0f));
        OLO_CORE_INFO("  Hit Ratio: {:.1f}% ({}/{} requests)", 
                      GetCacheHitRatio() * 100.0f, m_HitCount, m_HitCount + m_MissCount);
    }

    void CompilerCache::SetCacheDirectory(const std::string& directory)
    {
        OLO_PROFILE_FUNCTION();

        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            if (m_CacheDirectory != directory)
            {
                // Save current cache before switching
                if (m_AutoSave && !m_CompiledResults.empty())
                {
                    SaveToDisk();
                }
                
                m_CacheDirectory = directory;
                CreateCacheDirectory();
                
                // Clear in-memory cache and reload from new directory
                m_CompiledResults.clear();
            }
        } // Lock is released here
        
        // Reload from disk without holding the lock (profiled for telemetry)
        {
            OLO_PROFILE_SCOPE("CompilerCache::LoadFromDisk");
            LoadFromDisk();
        }
    }

    //==============================================================================
    /// Private Helper Methods

    std::string CompilerCache::GenerateCacheKey(const std::string& sourcePath, const std::string& compilerVersion) const
    {
        OLO_PROFILE_FUNCTION();

        return sourcePath + "|" + compilerVersion;
    }

    sizet CompilerCache::HashString(const std::string& str) const
    {
        OLO_PROFILE_FUNCTION();

        sizet hash = 0;
        for (char c : str)
        {
            hash = hash * 31 + static_cast<sizet>(c);
        }
        return hash;
    }

    sizet CompilerCache::GetFileSize(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();

        std::error_code ec;
        auto size = std::filesystem::file_size(filePath, ec);
        return ec ? 0 : static_cast<sizet>(size);
    }

    std::chrono::time_point<std::chrono::system_clock> CompilerCache::GetFileModificationTime(const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();

        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(filePath, ec);
        if (ec)
            return std::chrono::system_clock::now();

        // Convert to system_clock time_point
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return sctp;
    }

    bool CompilerCache::CreateCacheDirectory() const
    {
        OLO_PROFILE_FUNCTION();

        try
        {
            if (!std::filesystem::exists(m_CacheDirectory))
            {
                std::filesystem::create_directories(m_CacheDirectory);
            }
            return true;
        }
        catch (const std::filesystem::filesystem_error& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Failed to create cache directory '{}': {}", m_CacheDirectory, ex.what());
            return false;
        }
    }

    bool CompilerCache::SerializeResult(const CompilationResult& result, const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();

        try
        {
            std::ofstream file(filePath, std::ios::binary);
            if (!file.is_open())
                return false;

            // Write magic header and format version for future compatibility
            // Magic: "OLCC" (OloEngine Compiler Cache) - 4 bytes
            // Version: 1 (u32) - Current format version
            // Increment version when fields are added/reordered for backward/forward compatibility
            const char magic[4] = {'O', 'L', 'C', 'C'};
            const u32 formatVersion = 1;
            
            file.write(magic, sizeof(magic));
            file.write(reinterpret_cast<const char*>(&formatVersion), sizeof(formatVersion));

            // Simple binary serialization (in production, use a proper serialization library)
            auto writeString = [&file](const std::string& str) {
                u32 length = static_cast<u32>(str.size());
                file.write(reinterpret_cast<const char*>(&length), sizeof(length));
                file.write(str.c_str(), length);
            };

            writeString(result.m_SourcePath);
            writeString(result.m_CompiledPath);
            
            u32 dataSize = static_cast<u32>(result.m_CompiledData.size());
            file.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
            file.write(reinterpret_cast<const char*>(result.m_CompiledData.data()), dataSize);
            
            file.write(reinterpret_cast<const char*>(&result.m_SourceHash), sizeof(result.m_SourceHash));
            
            auto timePoint = result.m_CompilationTime.time_since_epoch().count();
            file.write(reinterpret_cast<const char*>(&timePoint), sizeof(timePoint));
            
            writeString(result.m_CompilerVersion);
            writeString(result.m_ErrorMessage);
            
            file.write(reinterpret_cast<const char*>(&result.m_IsValid), sizeof(result.m_IsValid));
            file.write(reinterpret_cast<const char*>(&result.m_CompilationTimeMs), sizeof(result.m_CompilationTimeMs));
            file.write(reinterpret_cast<const char*>(&result.m_SourceSizeBytes), sizeof(result.m_SourceSizeBytes));
            file.write(reinterpret_cast<const char*>(&result.m_CompiledSizeBytes), sizeof(result.m_CompiledSizeBytes));

            return true;
        }
        catch (const std::exception& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Failed to serialize result to '{}': {}", filePath, ex.what());
            return false;
        }
    }

    bool CompilerCache::DeserializeResult(CompilationResult& result, const std::string& filePath) const
    {
        OLO_PROFILE_FUNCTION();
        
        try
        {
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open())
                return false;

            // Validate magic header and format version
            char magic[4];
            file.read(magic, sizeof(magic));
            if (magic[0] != 'O' || magic[1] != 'L' || magic[2] != 'C' || magic[3] != 'C')
            {
                OLO_CORE_ERROR("CompilerCache: Invalid magic header in cache file '{}'", filePath);
                return false;
            }
            
            u32 formatVersion;
            file.read(reinterpret_cast<char*>(&formatVersion), sizeof(formatVersion));
            if (formatVersion > 1) // Current supported version is 1
            {
                OLO_CORE_WARN("CompilerCache: Unsupported format version {} in cache file '{}' (expected <= 1)", formatVersion, filePath);
                return false;
            }
            if (formatVersion < 1) // Minimum supported version is 1
            {
                OLO_CORE_WARN("CompilerCache: Outdated format version {} in cache file '{}' (expected >= 1)", formatVersion, filePath);
                return false;
            }

            auto readString = [&file]() -> std::string {
                u32 length;
                file.read(reinterpret_cast<char*>(&length), sizeof(length));
                std::string str(length, '\0');
                file.read(&str[0], length);
                return str;
            };

            result.m_SourcePath = readString();
            result.m_CompiledPath = readString();
            
            u32 dataSize;
            file.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize));
            result.m_CompiledData.resize(dataSize);
            file.read(reinterpret_cast<char*>(result.m_CompiledData.data()), dataSize);
            
            file.read(reinterpret_cast<char*>(&result.m_SourceHash), sizeof(result.m_SourceHash));
            
            decltype(result.m_CompilationTime.time_since_epoch().count()) timePoint;
            file.read(reinterpret_cast<char*>(&timePoint), sizeof(timePoint));
            result.m_CompilationTime = std::chrono::system_clock::time_point(std::chrono::system_clock::duration(timePoint));
            
            result.m_CompilerVersion = readString();
            result.m_ErrorMessage = readString();
            
            file.read(reinterpret_cast<char*>(&result.m_IsValid), sizeof(result.m_IsValid));
            file.read(reinterpret_cast<char*>(&result.m_CompilationTimeMs), sizeof(result.m_CompilationTimeMs));
            file.read(reinterpret_cast<char*>(&result.m_SourceSizeBytes), sizeof(result.m_SourceSizeBytes));
            file.read(reinterpret_cast<char*>(&result.m_CompiledSizeBytes), sizeof(result.m_CompiledSizeBytes));

            return true;
        }
        catch (const std::exception& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Failed to deserialize result from '{}': {}", filePath, ex.what());
            return false;
        }
    }

    //==============================================================================
    /// Compiler Utilities

    namespace CompilerUtilities
    {
        static OloEngine::Ref<OloEngine::Audio::SoundGraph::CompilerCache> s_GlobalCompilerCache;
        static std::mutex s_GlobalCacheMutex;

        OloEngine::Ref<OloEngine::Audio::SoundGraph::CompilerCache> GetGlobalCompilerCache()
        {
            std::lock_guard<std::mutex> lock(s_GlobalCacheMutex);
            
            if (!s_GlobalCompilerCache)
            {
                // Use default cache directory consistent with CompilerCacheConfig
                s_GlobalCompilerCache = OloEngine::Ref<OloEngine::Audio::SoundGraph::CompilerCache>::Create("cache/compiler/");
            }
            
            return s_GlobalCompilerCache;
        }

        void SetGlobalCompilerCache(OloEngine::Ref<OloEngine::Audio::SoundGraph::CompilerCache> cache)
        {
            std::lock_guard<std::mutex> lock(s_GlobalCacheMutex);
            s_GlobalCompilerCache = cache;
        }

        void InitializeCompilerCache()
        {
            OLO_PROFILE_FUNCTION();
            
            auto cache = GetGlobalCompilerCache();
            OLO_CORE_INFO("CompilerCache: Initialized global compiler cache");
        }

        void ShutdownCompilerCache()
        {
            OLO_PROFILE_FUNCTION();
            
            std::lock_guard<std::mutex> lock(s_GlobalCacheMutex);
            
            if (s_GlobalCompilerCache)
            {
                s_GlobalCompilerCache->LogStatistics();
                if (s_GlobalCompilerCache->GetAutoSave())
                {
                    s_GlobalCompilerCache->SaveToDisk();
                }
                s_GlobalCompilerCache.Reset();
                OLO_CORE_INFO("CompilerCache: Shutdown global compiler cache");
            }
        }

        OloEngine::Audio::SoundGraph::CompilationResult CompileWithCache(const std::string& sourcePath, const std::string& compilerVersion)
        {
            OLO_PROFILE_FUNCTION();
            
            auto cache = GetGlobalCompilerCache();
            
            // Check cache first
            if (auto cached = cache->GetCompiled(sourcePath, compilerVersion))
            {
                return *cached;
            }

            // Cache miss - need to compile
            OloEngine::Audio::SoundGraph::CompilationResult result;
            result.m_SourcePath = sourcePath;
            result.m_CompilerVersion = compilerVersion;
            result.m_CompilationTime = std::chrono::system_clock::now();
            
            auto startTime = std::chrono::high_resolution_clock::now();
            
            // ========================================================================
            // TODO: Implement DSP/Audio Script Compilation
            // ========================================================================
            // This placeholder is for a future bytecode compilation system for DSP code.
            // 
            // CURRENT ARCHITECTURE:
            // The SoundGraph system currently uses a different "compilation" pipeline:
            // 1. SoundGraphSerializer deserializes SoundGraphAsset from YAML/JSON
            // 2. GraphGenerator::ConstructPrototype() builds a Prototype from asset data
            // 3. CreateInstance() creates an executable SoundGraph from the Prototype
            // 4. The CompiledPrototype is cached in the SoundGraphAsset for faster loading
            // 
            // FUTURE IMPLEMENTATION:
            // When we add DSP scripting support (e.g., Lua/ChaiScript for custom audio nodes),
            // this function should:
            // 1. Read the DSP source code from sourcePath
            // 2. Parse and validate the script syntax
            // 3. Compile to bytecode (or JIT compile to native code)
            // 4. Package the bytecode with metadata (entry points, parameter definitions, etc.)
            // 5. Return the compiled bytecode in result.m_CompiledData
            // 
            // BYTECODE FORMAT (proposed):
            // - Header: Magic number, version, entry point offsets
            // - Symbol table: Parameter names, types, default values
            // - Code section: Compiled instructions or IR
            // - Metadata: Source hash, compilation flags, optimization level
            // 
            // INTEGRATION POINTS:
            // - Custom DSP node registration system
            // - Hot-reload support for script changes (using IsSourceNewer)
            // - Performance profiling for compiled vs interpreted execution
            // 
            // NOTES:
            // - This is NOT needed for the current graph-based audio system
            // - CompilerCache infrastructure is ready for when we add DSP scripting
            // - Consider LLVM/JIT compilation for optimal performance
            // - May want to support multiple DSP languages (Faust, SOUL, custom)
            // ========================================================================
            
            // Placeholder compilation (returns dummy bytecode until DSP system is implemented)
            result.m_IsValid = true;
            result.m_ErrorMessage = "";
            result.m_CompiledData = { 0x42, 0x43, 0x44, 0x45 }; // Placeholder bytecode
            
            auto endTime = std::chrono::high_resolution_clock::now();
            result.m_CompilationTimeMs = std::chrono::duration<f64, std::milli>(endTime - startTime).count();
            result.m_SourceSizeBytes = cache->GetFileSize(sourcePath);
            result.m_CompiledSizeBytes = result.m_CompiledData.size();
            
            // Store in cache
            cache->StoreCompiled(sourcePath, result);
            
            return result;
        }

        std::vector<OloEngine::Audio::SoundGraph::CompilationResult> BatchCompileWithCache(
            const std::vector<std::string>& sourcePaths, 
            const std::string& compilerVersion)
        {
            OLO_PROFILE_FUNCTION();
            
            std::vector<OloEngine::Audio::SoundGraph::CompilationResult> results;
            results.reserve(sourcePaths.size());
            
            for (const std::string& path : sourcePaths)
            {
                results.push_back(CompileWithCache(path, compilerVersion));
            }
            
            return results;
        }

        void PerformMaintenanceTasks()
        {
            OLO_PROFILE_FUNCTION();
            
            auto cache = GetGlobalCompilerCache();
            cache->CompactCache();
        }

        void CleanupExpiredEntries()
        {
            OLO_PROFILE_FUNCTION();
            
            auto cache = GetGlobalCompilerCache();
            cache->CleanupOldEntries();
        }

        void ValidateAllCaches()
        {
            OLO_PROFILE_FUNCTION();
            
            auto cache = GetGlobalCompilerCache();
            cache->ValidateAllEntries();
        }
    }

} // namespace OloEngine::Audio::SoundGraph