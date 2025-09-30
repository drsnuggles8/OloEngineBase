#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

// Default compiler version - can be overridden at compile time
#ifndef OLO_SOUND_GRAPH_COMPILER_VERSION
#define OLO_SOUND_GRAPH_COMPILER_VERSION "v0.0.1"
#endif

namespace OloEngine::Audio::SoundGraph
{
    /// Compilation result with metadata
    struct CompilationResult
    {
        std::string SourcePath;
        std::string CompiledPath;
        std::vector<u8> CompiledData;
        sizet SourceHash = 0;
        std::chrono::time_point<std::chrono::system_clock> CompilationTime;
        std::string CompilerVersion;
        bool IsValid = false;
        std::string ErrorMessage;
        
        // Compilation statistics
        f64 CompilationTimeMs = 0.0;
        sizet SourceSizeBytes = 0;
        sizet CompiledSizeBytes = 0;
    };

    /// High-performance cache for compiled sound graph bytecode
    class CompilerCache : public RefCounted
    {
    public:
        CompilerCache(const std::string& cacheDirectory = "cache/compiler/");
        ~CompilerCache() = default;

        /// Cache Operations
        bool HasCompiled(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION) const;
        std::shared_ptr<const CompilationResult> GetCompiled(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION) const;
        void StoreCompiled(const std::string& sourcePath, const CompilationResult& result);
        void InvalidateCompiled(const std::string& sourcePath);
        void InvalidateCompiled(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION);
        void ClearCache();

        /// File System Integration
        bool IsSourceNewer(const std::string& sourcePath, const CompilationResult& result) const;
        std::string GetCacheFilePath(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION) const;
        
        /// Persistent Storage
        bool LoadFromDisk();
        bool SaveToDisk() const;
        void SetAutoSave(bool enabled) 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_AutoSave = enabled; 
        }
        
        bool GetAutoSave() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_AutoSave; 
        }
        
        /// Cache Management
        void ValidateAllEntries();
        void CleanupOldEntries(std::chrono::hours maxAge = std::chrono::hours(24 * 7)); // 1 week default
        void CompactCache();
        
        /// Statistics
        sizet GetCacheSize() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_CompiledResults.size(); 
        }
        sizet GetTotalDiskUsage() const;
        f32 GetCacheHitRatio() const;
        void LogStatistics() const;
        
        /// Configuration
        void SetCacheDirectory(const std::string& directory);
        std::string GetCacheDirectory() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_CacheDirectory; 
        }
        void SetMaxCacheSize(sizet maxSize) 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_MaxCacheSize = maxSize; 
        }
        sizet GetMaxCacheSize() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_MaxCacheSize; 
        }
        
        /// Initialization Status
        bool IsFullyInitialized() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_DirectoryInitialized && m_DiskCacheLoaded; 
        }

        bool IsDirectoryInitialized() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_DirectoryInitialized; 
        }

        bool IsDiskCacheLoaded() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_DiskCacheLoaded; 
        }

        std::string GetInitializationErrors() const 
        { 
            std::lock_guard<std::mutex> lock(m_Mutex);
            return m_InitializationErrors; 
        }
        
        sizet GetFileSize(const std::string& filePath) const;
        
    private:
        std::chrono::time_point<std::chrono::system_clock> GetFileModificationTime(const std::string& filePath) const;
        bool CreateCacheDirectory() const;
        
        // Serialization
        bool SerializeResult(const CompilationResult& result, const std::string& filePath) const;
        bool DeserializeResult(CompilationResult& result, const std::string& filePath) const;
        
    private:
        mutable std::mutex m_Mutex;
        std::unordered_map<std::string, std::shared_ptr<CompilationResult>> m_CompiledResults;
        
        std::string m_CacheDirectory;
        sizet m_MaxCacheSize = 1000; // Maximum number of cached compilations
        bool m_AutoSave = true;
        
        // Statistics
        mutable u64 m_HitCount = 0;
        mutable u64 m_MissCount = 0;
        
        // Initialization state tracking
        bool m_DirectoryInitialized = false;
        bool m_DiskCacheLoaded = false;
        std::string m_InitializationErrors;
        
        // Helper methods
        std::string GenerateCacheKey(const std::string& sourcePath, const std::string& compilerVersion) const;
        sizet HashString(const std::string& str) const;
    };

    /// Compiler integration utilities
    namespace CompilerUtilities
    {
        /// Get or create global compiler cache instance
        Ref<CompilerCache> GetGlobalCompilerCache();
        
        /// Set custom compiler cache instance
        void SetGlobalCompilerCache(Ref<CompilerCache> cache);
        
        /// Initialize compiler cache system
        void InitializeCompilerCache();
        
        /// Shutdown compiler cache system
        void ShutdownCompilerCache();
        
        /// Compile with caching
        CompilationResult CompileWithCache(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION);
        
        /// Batch compilation utilities
        std::vector<CompilationResult> BatchCompileWithCache(
            const std::vector<std::string>& sourcePaths, 
            const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION);
        
        /// Cache maintenance
        void PerformMaintenanceTasks();
        void CleanupExpiredEntries();
        void ValidateAllCaches();
    }

    //==============================================================================
    /// Configuration structure for compiler cache
    
    struct CompilerCacheConfig
    {
        std::string CacheDirectory = "cache/compiler/";
        sizet MaxCacheSize = 1000;
        bool AutoSave = true;
        bool EnableDiskCache = true;
        std::chrono::hours MaxEntryAge = std::chrono::hours(24 * 7); // 1 week
        sizet MaxDiskUsageBytes = 512 * 1024 * 1024; // 512MB
    };

} // namespace OloEngine::Audio::SoundGraph