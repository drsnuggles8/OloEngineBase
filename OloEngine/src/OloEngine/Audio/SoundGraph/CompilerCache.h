#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Task/Task.h"

#include <chrono>
#include <memory>
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <queue>
#include <condition_variable>
#include <atomic>

// Default compiler version - can be overridden at compile time
#ifndef OLO_SOUND_GRAPH_COMPILER_VERSION
#define OLO_SOUND_GRAPH_COMPILER_VERSION "v0.0.1"
#endif

namespace OloEngine::Audio::SoundGraph
{
    /// Compilation result with metadata
    struct CompilationResult
    {
        std::string m_SourcePath;
        std::string m_CompiledPath;
        std::vector<u8> m_CompiledData;
        sizet m_SourceHash = 0;
        std::chrono::time_point<std::chrono::system_clock> m_CompilationTime;
        std::string m_CompilerVersion;
        bool m_IsValid = false;
        std::string m_ErrorMessage;

        // Compilation statistics
        f32 m_CompilationTimeMs = 0.0f;
        sizet m_SourceSizeBytes = 0;
        sizet m_CompiledSizeBytes = 0;
    };

    /// High-performance cache for compiled sound graph bytecode
    class CompilerCache : public RefCounted
    {
      public:
        CompilerCache(const std::string& cacheDirectory = "cache/compiler/");
        ~CompilerCache();

        /// Cache Operations
        bool HasCompiled(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION) const;
        std::shared_ptr<const CompilationResult> GetCompiled(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION) const;
        void StoreCompiled(const std::string& sourcePath, const CompilationResult& result);
        void InvalidateCompiled(const std::string& sourcePath);
        void InvalidateCompiled(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION);

        /// Clears all in-memory cached compilation results and optionally deletes the disk cache.
        /// WARNING: This is a DESTRUCTIVE operation when force=true - all cached files will be permanently deleted!
        /// @param force If true, physically deletes the cache directory from disk and recreates it.
        ///              If false (default), only clears in-memory cache without touching disk files.
        ///              A warning is always logged before disk deletion to prevent accidental data loss.
        /// @param allowDeletionWithoutBackup If true, allows deletion even if backup creation fails.
        ///                                    If false (default), aborts deletion when backup fails to prevent data loss.
        void ClearCache(bool force = false, bool allowDeletionWithoutBackup = false);

        /// File System Integration
        bool IsSourceNewer(const std::string& sourcePath, const CompilationResult& result) const;
        std::string GetCacheFilePath(const std::string& sourcePath, const std::string& compilerVersion = OLO_SOUND_GRAPH_COMPILER_VERSION) const;

        /// Persistent Storage
        bool LoadFromDisk();
        bool SaveToDisk() const;
        void SetAutoSave(bool enabled)
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            m_AutoSave = enabled;
        }

        bool GetAutoSave() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_AutoSave;
        }

        /// Cache Management
        void ValidateAllEntries();
        void CleanupOldEntries(std::chrono::hours maxAge = std::chrono::hours(24 * 7)); // 1 week default
        void CompactCache();

        /// Statistics
        sizet GetCacheSize() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_CompiledResults.size();
        }
        sizet GetTotalDiskUsage() const;
        f32 GetCacheHitRatio() const;
        void LogStatistics() const;

        /// Configuration
        void SetCacheDirectory(const std::string& directory);
        std::string GetCacheDirectory() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_CacheDirectory;
        }

        void SetMaxCacheSize(sizet maxSize)
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            m_MaxCacheSize = maxSize;
        }

        sizet GetMaxCacheSize() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_MaxCacheSize;
        }

        /// Initialization Status
        bool IsFullyInitialized() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_DirectoryInitialized && m_DiskCacheLoaded;
        }

        bool IsDirectoryInitialized() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_DirectoryInitialized;
        }

        bool IsDiskCacheLoaded() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_DiskCacheLoaded;
        }

        std::string GetInitializationErrors() const
        {
            TUniqueLock<FMutex> lock(m_Mutex);
            return m_InitializationErrors;
        }

        sizet GetFileSize(const std::string& filePath) const;

      private:
        std::chrono::time_point<std::chrono::system_clock> GetFileModificationTime(const std::string& filePath) const;
        bool CreateCacheDirectory() const;

        // Serialization
        bool SerializeResult(const CompilationResult& result, const std::string& filePath) const;
        bool DeserializeResult(CompilationResult& result, const std::string& filePath) const;

        // Async save support
        struct SaveTask
        {
            CompilationResult m_Result;
            std::string m_FilePath;
        };

        void AsyncSaveWorker();
        void EnqueueSave(const CompilationResult& result, const std::string& filePath);
        void ShutdownAsyncSaver();

      private:
        mutable FMutex m_Mutex;
        std::unordered_map<std::string, std::shared_ptr<CompilationResult>> m_CompiledResults;

        // LRU tracking: front = oldest (LRU), back = newest (MRU)
        mutable std::list<std::string> m_AccessOrder;
        mutable std::unordered_map<std::string, std::list<std::string>::iterator> m_AccessOrderMap;

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

        // Async save worker (using Task System)
        std::queue<SaveTask> m_SaveQueue;
        FMutex m_SaveQueueMutex;
        std::atomic<bool> m_ShuttingDown{ false };
        std::atomic<u32> m_ActiveSaveTasks{ 0 };

        // Helper methods
        std::string GenerateCacheKey(const std::string& sourcePath, const std::string& compilerVersion) const;
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
    } // namespace CompilerUtilities

    //==============================================================================
    /// Configuration structure for compiler cache

    struct CompilerCacheConfig
    {
        std::string m_CacheDirectory = "cache/compiler/";
        sizet m_MaxCacheSize = 1000;
        bool m_AutoSave = true;
        bool m_EnableDiskCache = true;
        std::chrono::hours m_MaxEntryAge = std::chrono::hours(24 * 7); // 1 week
        sizet m_MaxDiskUsageBytes = 512 * 1024 * 1024;                 // 512MB
    };

} // namespace OloEngine::Audio::SoundGraph
