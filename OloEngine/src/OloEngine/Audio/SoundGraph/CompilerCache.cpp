#include "OloEnginePCH.h"
#include "CompilerCache.h"
#include "OloEngine/Project/Project.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// CompilerCache Implementation

    CompilerCache::CompilerCache(const std::string& cacheDirectory)
        : m_CacheDirectory(cacheDirectory)
    {
        OLO_PROFILE_FUNCTION();

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

        // Start async save worker thread
        m_SaveWorkerRunning = true;
        try
        {
            m_SaveWorkerThread = std::thread(&CompilerCache::AsyncSaveWorker, this);
            OLO_CORE_TRACE("CompilerCache: Async save worker thread started");
        }
        catch (const std::system_error& e)
        {
            m_SaveWorkerRunning = false;
            OLO_CORE_ERROR("CompilerCache: Failed to create async save worker thread: {} (error code: {})",
                           e.what(), e.code().value());
        }
        catch (const std::exception& e)
        {
            m_SaveWorkerRunning = false;
            OLO_CORE_ERROR("CompilerCache: Failed to create async save worker thread: {}", e.what());
        }
    }

    CompilerCache::~CompilerCache()
    {
        ShutdownAsyncSaver();
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

        // Update LRU: move accessed key to back (most recently used)
        auto orderIt = m_AccessOrderMap.find(key);
        if (orderIt != m_AccessOrderMap.end())
        {
            m_AccessOrder.erase(orderIt->second);
            m_AccessOrder.push_back(key);
            m_AccessOrderMap[key] = std::prev(m_AccessOrder.end());
        }

        return it->second; // Return shared_ptr for thread-safe access
    }

    void CompilerCache::StoreCompiled(const std::string& sourcePath, const CompilationResult& result)
    {
        OLO_PROFILE_FUNCTION();

        std::lock_guard<std::mutex> lock(m_Mutex);

        std::string key = GenerateCacheKey(sourcePath, result.m_CompilerVersion);

        // Check if we need to evict old entries
        if (m_CompiledResults.size() >= m_MaxCacheSize)
        {
            // First, remove any nullptr entries to prevent invalid comparisons
            std::erase_if(m_CompiledResults, [this](const auto& entry)
                          {
                if (entry.second == nullptr)
                {
                    // Also remove from LRU tracking
                    auto orderIt = m_AccessOrderMap.find(entry.first);
                    if (orderIt != m_AccessOrderMap.end())
                    {
                        m_AccessOrder.erase(orderIt->second);
                        m_AccessOrderMap.erase(orderIt);
                    }
                    return true;
                }
                return false; });

            // If we still need to evict after removing nullptrs, remove the LRU entry
            if (m_CompiledResults.size() >= m_MaxCacheSize && !m_AccessOrder.empty())
            {
                // Get the least recently used key (front of the list)
                const std::string& lruKey = m_AccessOrder.front();

                // Look up the entry for logging before erasing
                auto entryIt = m_CompiledResults.find(lruKey);
                if (entryIt != m_CompiledResults.end() && entryIt->second)
                {
                    OLO_CORE_TRACE("CompilerCache: Evicting LRU entry (source: '{}', compiler: '{}')",
                                   entryIt->second->m_SourcePath, entryIt->second->m_CompilerVersion);
                }

                // Remove from all tracking structures
                m_CompiledResults.erase(lruKey);
                m_AccessOrderMap.erase(lruKey);
                m_AccessOrder.pop_front();
            }
        }

        // If key already exists, remove it from LRU tracking (will be re-added at back)
        auto existingOrderIt = m_AccessOrderMap.find(key);
        if (existingOrderIt != m_AccessOrderMap.end())
        {
            m_AccessOrder.erase(existingOrderIt->second);
            m_AccessOrderMap.erase(existingOrderIt);
        }

        // Store the compilation result
        m_CompiledResults[key] = std::make_shared<CompilationResult>(result);

        // Add to LRU tracking (most recently used = back of list)
        m_AccessOrder.push_back(key);
        m_AccessOrderMap[key] = std::prev(m_AccessOrder.end());

        if (m_AutoSave)
        {
            // Save to disk asynchronously using background worker thread
            std::string filePath = GetCacheFilePath(sourcePath, result.m_CompilerVersion);
            EnqueueSave(result, filePath);
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

    void CompilerCache::ClearCache(bool force, bool allowDeletionWithoutBackup)
    {
        OLO_PROFILE_FUNCTION();

        std::lock_guard<std::mutex> lock(m_Mutex);

        // Always clear in-memory cache
        m_CompiledResults.clear();
        m_AccessOrder.clear();
        m_AccessOrderMap.clear();
        m_HitCount = 0;
        m_MissCount = 0;

        OLO_CORE_INFO("CompilerCache: Cleared in-memory cache (hit count: 0, miss count: 0)");

        // Handle disk cache deletion if forced
        if (!force)
        {
            OLO_CORE_INFO("CompilerCache: Disk cache preserved (use ClearCache(true) to delete disk files)");
            return;
        }

        // DESTRUCTIVE OPERATION: Delete disk cache
        try
        {
            if (!std::filesystem::exists(m_CacheDirectory))
            {
                OLO_CORE_INFO("CompilerCache: Cache directory '{}' does not exist, nothing to clear", m_CacheDirectory);
                return;
            }

            // Create backup directory path with timestamp
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            std::filesystem::path backupPath = std::filesystem::path(m_CacheDirectory).parent_path() /
                                               ("compiler_backup_" + std::to_string(timestamp));

            // Log prominent warning before destructive operation
            OLO_CORE_WARN("CompilerCache: DESTRUCTIVE OPERATION - Deleting disk cache at '{}'", m_CacheDirectory);
            OLO_CORE_WARN("CompilerCache: Creating backup at '{}' before deletion", backupPath.string());

            // Attempt to create backup before deletion
            bool backupSuccess = false;
            try
            {
                std::filesystem::copy(m_CacheDirectory, backupPath,
                                      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
                backupSuccess = true;
                OLO_CORE_INFO("CompilerCache: Backup created successfully at '{}'", backupPath.string());
            }
            catch (const std::filesystem::filesystem_error& backupEx)
            {
                OLO_CORE_ERROR("CompilerCache: Failed to create backup: {} (path: '{}', error code: {})",
                               backupEx.what(), backupEx.path1().string(), backupEx.code().value());
            }

            // SAFETY CHECK: Abort deletion if backup failed unless explicitly allowed
            if (!backupSuccess && !allowDeletionWithoutBackup)
            {
                OLO_CORE_ERROR("CompilerCache: ABORTING cache deletion - backup creation failed and allowDeletionWithoutBackup is false");
                OLO_CORE_ERROR("CompilerCache: To proceed with deletion despite backup failure, call ClearCache(true, true)");
                OLO_CORE_INFO("CompilerCache: In-memory cache was cleared, but disk cache remains intact at '{}'", m_CacheDirectory);
                return;
            }

            if (!backupSuccess && allowDeletionWithoutBackup)
            {
                OLO_CORE_WARN("CompilerCache: Proceeding with deletion WITHOUT backup (allowDeletionWithoutBackup=true) - DATA LOSS RISK!");
            }

            // Perform the destructive removal
            std::error_code removeEc;
            auto removedCount = std::filesystem::remove_all(m_CacheDirectory, removeEc);

            if (removeEc)
            {
                OLO_CORE_ERROR("CompilerCache: Failed to remove cache directory '{}': {} (error code: {})",
                               m_CacheDirectory, removeEc.message(), removeEc.value());

                if (backupSuccess)
                {
                    OLO_CORE_INFO("CompilerCache: Backup is available at '{}' for recovery", backupPath.string());
                }
                return;
            }

            OLO_CORE_INFO("CompilerCache: Successfully deleted {} files/directories from '{}'", removedCount, m_CacheDirectory);

            if (backupSuccess)
            {
                OLO_CORE_INFO("CompilerCache: Backup preserved at '{}' (can be manually deleted if not needed)", backupPath.string());
            }

            // Recreate empty cache directory
            CreateCacheDirectory();
            OLO_CORE_INFO("CompilerCache: Recreated empty cache directory at '{}'", m_CacheDirectory);
        }
        catch (const std::filesystem::filesystem_error& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Filesystem error during cache clearing: {} (path: '{}', error code: {})",
                           ex.what(), ex.path1().string(), ex.code().value());
        }
        catch (const std::exception& ex)
        {
            OLO_CORE_ERROR("CompilerCache: Unexpected error during cache clearing: {}", ex.what());
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

        // Use std::filesystem::path for cross-platform path concatenation
        std::filesystem::path cachePath = std::filesystem::path(m_CacheDirectory) / (key + ".compiled");
        return cachePath.string();
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

                        // Initialize LRU tracking for loaded entries
                        m_AccessOrder.push_back(key);
                        m_AccessOrderMap[key] = std::prev(m_AccessOrder.end());

                        ++loadedCount;
                    }
                    else
                    {
                        // Log failed deserialization for debugging and auditing
                        OLO_CORE_WARN("CompilerCache: Failed to deserialize compiled cache file: '{}' - file may be corrupted or incompatible",
                                      entry.path().string());
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
            sizet failedCount = 0;

            for (const auto& [key, resultPtr] : m_CompiledResults)
            {
                if (resultPtr && resultPtr->m_IsValid)
                {
                    std::string filePath = GetCacheFilePath(resultPtr->m_SourcePath, resultPtr->m_CompilerVersion);
                    if (SerializeResult(*resultPtr, filePath))
                    {
                        ++savedCount;
                    }
                    else
                    {
                        // Log detailed failure information for debugging
                        OLO_CORE_WARN("CompilerCache: Failed to serialize cache entry to '{}' (source: '{}', compiler: '{}')",
                                      filePath, resultPtr->m_SourcePath, resultPtr->m_CompilerVersion);
                        ++failedCount;
                    }
                }
            }

            if (failedCount > 0)
            {
                OLO_CORE_INFO("CompilerCache: Saved {} compiled results to disk ({} failed)", savedCount, failedCount);
            }
            else
            {
                OLO_CORE_INFO("CompilerCache: Saved {} compiled results to disk", savedCount);
            }

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

        // Generate a deterministic hash-based cache key to avoid collisions from separator characters
        // Uses hash_combine pattern to mix hashes of both inputs into a single deterministic value
        std::hash<std::string> hasher;

        // Compute individual hashes
        sizet h1 = hasher(sourcePath);
        sizet h2 = hasher(compilerVersion);

        // Combine hashes using the hash_combine algorithm (similar to boost::hash_combine)
        // This approach provides good distribution and is deterministic across runs
        sizet combinedHash = h1;
        combinedHash ^= h2 + 0x9e3779b9 + (combinedHash << 6) + (combinedHash >> 2);

        // Convert to hexadecimal string for filesystem-safe cache key
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(sizeof(sizet) * 2) << combinedHash;
        return oss.str();
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

            // Platform-independent binary serialization helpers (little-endian byte order)
            // All multi-byte numeric values are converted to little-endian before writing
            auto write_u32 = [&file](u32 value)
            {
                // Convert to little-endian byte order
                u8 bytes[4];
                bytes[0] = static_cast<u8>(value & 0xFF);
                bytes[1] = static_cast<u8>((value >> 8) & 0xFF);
                bytes[2] = static_cast<u8>((value >> 16) & 0xFF);
                bytes[3] = static_cast<u8>((value >> 24) & 0xFF);
                file.write(reinterpret_cast<const char*>(bytes), 4);
            };

            auto write_u64 = [&file](u64 value)
            {
                // Convert to little-endian byte order
                u8 bytes[8];
                bytes[0] = static_cast<u8>(value & 0xFF);
                bytes[1] = static_cast<u8>((value >> 8) & 0xFF);
                bytes[2] = static_cast<u8>((value >> 16) & 0xFF);
                bytes[3] = static_cast<u8>((value >> 24) & 0xFF);
                bytes[4] = static_cast<u8>((value >> 32) & 0xFF);
                bytes[5] = static_cast<u8>((value >> 40) & 0xFF);
                bytes[6] = static_cast<u8>((value >> 48) & 0xFF);
                bytes[7] = static_cast<u8>((value >> 56) & 0xFF);
                file.write(reinterpret_cast<const char*>(bytes), 8);
            };

            auto write_f64 = [&file, &write_u64](f64 value)
            {
                // Reinterpret double as u64 bit pattern, then write in little-endian
                u64 bits;
                std::memcpy(&bits, &value, sizeof(f64));
                write_u64(bits);
            };

            auto write_bool = [&file](bool value)
            {
                // Write boolean as single byte (platform-independent)
                u8 byte = value ? 1 : 0;
                file.write(reinterpret_cast<const char*>(&byte), 1);
            };

            // Write magic header and format version for future compatibility
            // Magic: "OLCC" (OloEngine Compiler Cache) - 4 bytes
            // Version: 2 (u32) - Current format version (bumped from 1 for little-endian format)
            // Increment version when fields are added/reordered for backward/forward compatibility
            const char magic[4] = { 'O', 'L', 'C', 'C' };
            const u32 formatVersion = 2;

            file.write(magic, sizeof(magic));
            write_u32(formatVersion);

            // Platform-independent string writer using little-endian length
            auto writeString = [&file, &write_u32](const std::string& str)
            {
                u32 length = static_cast<u32>(str.size());
                write_u32(length);
                file.write(str.c_str(), length);
            };

            writeString(result.m_SourcePath);
            writeString(result.m_CompiledPath);

            u32 dataSize = static_cast<u32>(result.m_CompiledData.size());
            write_u32(dataSize);
            file.write(reinterpret_cast<const char*>(result.m_CompiledData.data()), dataSize);

            write_u64(result.m_SourceHash);

            // Convert time_point to fixed-size u64 before writing
            u64 timePoint = static_cast<u64>(result.m_CompilationTime.time_since_epoch().count());
            write_u64(timePoint);

            writeString(result.m_CompilerVersion);
            writeString(result.m_ErrorMessage);

            write_bool(result.m_IsValid);
            write_f64(result.m_CompilationTimeMs);
            write_u64(result.m_SourceSizeBytes);
            write_u64(result.m_CompiledSizeBytes);

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

            // Platform-independent binary deserialization helpers (little-endian byte order)
            auto read_u32 = [&file]() -> u32
            {
                u8 bytes[4];
                file.read(reinterpret_cast<char*>(bytes), 4);
                // Convert from little-endian to native byte order
                return static_cast<u32>(bytes[0]) |
                       (static_cast<u32>(bytes[1]) << 8) |
                       (static_cast<u32>(bytes[2]) << 16) |
                       (static_cast<u32>(bytes[3]) << 24);
            };

            auto read_u64 = [&file]() -> u64
            {
                u8 bytes[8];
                file.read(reinterpret_cast<char*>(bytes), 8);
                // Convert from little-endian to native byte order
                return static_cast<u64>(bytes[0]) |
                       (static_cast<u64>(bytes[1]) << 8) |
                       (static_cast<u64>(bytes[2]) << 16) |
                       (static_cast<u64>(bytes[3]) << 24) |
                       (static_cast<u64>(bytes[4]) << 32) |
                       (static_cast<u64>(bytes[5]) << 40) |
                       (static_cast<u64>(bytes[6]) << 48) |
                       (static_cast<u64>(bytes[7]) << 56);
            };

            auto read_f64 = [&file, &read_u64]() -> f64
            {
                // Read u64 bit pattern in little-endian, then reinterpret as double
                u64 bits = read_u64();
                f64 value;
                std::memcpy(&value, &bits, sizeof(f64));
                return value;
            };

            auto read_bool = [&file]() -> bool
            {
                u8 byte;
                file.read(reinterpret_cast<char*>(&byte), 1);
                return byte != 0;
            };

            // Validate magic header and format version
            char magic[4];
            file.read(magic, sizeof(magic));
            if (magic[0] != 'O' || magic[1] != 'L' || magic[2] != 'C' || magic[3] != 'C')
            {
                OLO_CORE_ERROR("CompilerCache: Invalid magic header in cache file '{}'", filePath);
                return false;
            }

            // Read format version using little-endian reader (matches write_u32 in SerializeResult)
            u32 formatVersion = read_u32();
            if (formatVersion > 2) // Current supported version is 2
            {
                OLO_CORE_WARN("CompilerCache: Unsupported format version {} in cache file '{}' (expected <= 2)", formatVersion, filePath);
                return false;
            }
            if (formatVersion < 1) // Minimum supported version is 1
            {
                OLO_CORE_WARN("CompilerCache: Outdated format version {} in cache file '{}' (expected >= 1)", formatVersion, filePath);
                return false;
            }

            // Platform-independent string reader using little-endian length
            auto readString = [&file, &read_u32, formatVersion]() -> std::string
            {
                u32 length;
                if (formatVersion == 1)
                {
                    // Old format: read platform-dependent length
                    file.read(reinterpret_cast<char*>(&length), sizeof(length));
                }
                else
                {
                    // New format (v2+): read little-endian length
                    length = read_u32();
                }
                std::string str(length, '\0');
                file.read(&str[0], length);
                return str;
            };

            result.m_SourcePath = readString();
            result.m_CompiledPath = readString();

            u32 dataSize;
            if (formatVersion == 1)
            {
                file.read(reinterpret_cast<char*>(&dataSize), sizeof(dataSize));
            }
            else
            {
                dataSize = read_u32();
            }
            result.m_CompiledData.resize(dataSize);
            file.read(reinterpret_cast<char*>(result.m_CompiledData.data()), dataSize);

            if (formatVersion == 1)
            {
                // Old format: platform-dependent reads
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
            }
            else // formatVersion >= 2
            {
                // New format: platform-independent little-endian reads
                result.m_SourceHash = read_u64();

                u64 timePoint = read_u64();
                result.m_CompilationTime = std::chrono::system_clock::time_point(
                    std::chrono::system_clock::duration(timePoint));

                result.m_CompilerVersion = readString();
                result.m_ErrorMessage = readString();

                result.m_IsValid = read_bool();
                result.m_CompilationTimeMs = static_cast<f32>(read_f64());
                result.m_SourceSizeBytes = read_u64();
                result.m_CompiledSizeBytes = read_u64();
            }

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
                // Derive absolute cache directory from project directory
                std::filesystem::path cacheDir;

                try
                {
                    // Use project directory as base if available, otherwise fall back to current working directory
                    if (Project::GetActive())
                    {
                        cacheDir = Project::GetProjectDirectory() / "cache" / "compiler";
                    }
                    else
                    {
                        // Fallback: use current working directory if no project is active
                        cacheDir = std::filesystem::current_path() / "cache" / "compiler";
                        OLO_CORE_WARN("CompilerCache: No active project found, using working directory for cache: {}", cacheDir.string());
                    }

                    // Convert to absolute path to ensure deterministic location
                    cacheDir = std::filesystem::absolute(cacheDir);

                    // Ensure the directory exists before passing to CompilerCache
                    std::filesystem::create_directories(cacheDir);

                    OLO_CORE_INFO("CompilerCache: Using absolute cache directory: {}", cacheDir.string());

                    s_GlobalCompilerCache = OloEngine::Ref<OloEngine::Audio::SoundGraph::CompilerCache>::Create(cacheDir.string());
                }
                catch (const std::filesystem::filesystem_error& ex)
                {
                    OLO_CORE_ERROR("CompilerCache: Failed to create cache directory: {}", ex.what());

                    // Create with empty path as fallback (cache will be disabled)
                    s_GlobalCompilerCache = OloEngine::Ref<OloEngine::Audio::SoundGraph::CompilerCache>::Create("");
                }
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
            result.m_CompilationTimeMs = static_cast<f32>(std::chrono::duration<f64, std::milli>(endTime - startTime).count());
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
    } // namespace CompilerUtilities

    //==============================================================================
    /// Async Save Implementation

    void CompilerCache::EnqueueSave(const CompilationResult& result, const std::string& filePath)
    {
        OLO_PROFILE_FUNCTION();

        SaveTask task;
        task.m_Result = result;
        task.m_FilePath = filePath;

        {
            std::lock_guard<std::mutex> lock(m_SaveQueueMutex);
            m_SaveQueue.push(std::move(task));
        }

        m_SaveQueueCV.notify_one();
        OLO_CORE_TRACE("CompilerCache: Enqueued async save for '{}' (queue size: {})",
                       result.m_SourcePath, m_SaveQueue.size());
    }

    void CompilerCache::AsyncSaveWorker()
    {
        OLO_PROFILE_FUNCTION();

        OLO_CORE_INFO("CompilerCache: Async save worker thread running");

        while (m_SaveWorkerRunning)
        {
            SaveTask task;

            {
                std::unique_lock<std::mutex> lock(m_SaveQueueMutex);

                // Wait for work or shutdown signal
                m_SaveQueueCV.wait(lock, [this]
                                   { return !m_SaveQueue.empty() || !m_SaveWorkerRunning; });

                // Exit if shutting down and queue is empty
                if (!m_SaveWorkerRunning && m_SaveQueue.empty())
                {
                    break;
                }

                // Get next task
                if (!m_SaveQueue.empty())
                {
                    task = std::move(m_SaveQueue.front());
                    m_SaveQueue.pop();
                }
                else
                {
                    continue;
                }
            }

            // Perform the save outside the lock to avoid blocking enqueuers
            try
            {
                if (!SerializeResult(task.m_Result, task.m_FilePath))
                {
                    OLO_CORE_ERROR("CompilerCache: Async save failed for '{}' to '{}'",
                                   task.m_Result.m_SourcePath, task.m_FilePath);
                }
                else
                {
                    OLO_CORE_TRACE("CompilerCache: Async save completed for '{}' to '{}'",
                                   task.m_Result.m_SourcePath, task.m_FilePath);
                }
            }
            catch (const std::exception& ex)
            {
                OLO_CORE_ERROR("CompilerCache: Exception during async save for '{}': {}",
                               task.m_Result.m_SourcePath, ex.what());
            }
            catch (...)
            {
                OLO_CORE_ERROR("CompilerCache: Unknown exception during async save for '{}'",
                               task.m_Result.m_SourcePath);
            }
        }

        OLO_CORE_INFO("CompilerCache: Async save worker thread shutting down");
    }

    void CompilerCache::ShutdownAsyncSaver()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_SaveWorkerRunning)
        {
            return; // Already shut down
        }

        OLO_CORE_INFO("CompilerCache: Shutting down async save worker...");

        // Signal worker to stop
        m_SaveWorkerRunning = false;
        m_SaveQueueCV.notify_all();

        // Wait for worker to finish processing remaining tasks
        if (m_SaveWorkerThread.joinable())
        {
            m_SaveWorkerThread.join();
        }

        // Check if any tasks were left unprocessed
        {
            std::lock_guard<std::mutex> lock(m_SaveQueueMutex);
            if (!m_SaveQueue.empty())
            {
                OLO_CORE_WARN("CompilerCache: {} save tasks were not processed during shutdown",
                              m_SaveQueue.size());
            }
        }

        OLO_CORE_INFO("CompilerCache: Async save worker shutdown complete");
    }

} // namespace OloEngine::Audio::SoundGraph
