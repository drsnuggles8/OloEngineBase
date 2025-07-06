#include "OloEnginePCH.h"
#include "ResourceHandleCache.h"
#include "OloEngine/Core/Log.h"
#include <algorithm>

namespace OloEngine
{
    ResourceHandleCache::ResourceHandleCache()
    {
        OLO_CORE_TRACE("ResourceHandleCache: Initialized");
    }

    ResourceHandleCache::~ResourceHandleCache()
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        m_CachedHandles.clear();
        OLO_CORE_TRACE("ResourceHandleCache: Destroyed");
    }

    CachedHandle* ResourceHandleCache::CacheHandle(const std::string& resourceName, u32 handle, 
                                                  ShaderResourceType type, sizet memorySize)
    {
        if (!m_CachingEnabled)
            return nullptr;

        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        // Check if handle already cached
        auto it = m_CachedHandles.find(resourceName);
        if (it != m_CachedHandles.end())
        {
            // Update existing handle
            auto& cachedHandle = it->second;
            cachedHandle->Handle = handle;
            cachedHandle->Type = type;
            cachedHandle->MemorySize = memorySize;
            cachedHandle->IsValid = true;
            cachedHandle->Touch();
            
            // Remove from invalidated set if present
            m_InvalidatedHandles.erase(resourceName);
            
            OLO_CORE_TRACE("ResourceHandleCache: Updated cached handle for '{0}' (Handle: {1})", 
                          resourceName, handle);
            return cachedHandle.get();
        }
        
        // Create new cached handle
        auto cachedHandle = std::make_unique<CachedHandle>(handle, type, resourceName);
        cachedHandle->MemorySize = memorySize;
        
        auto* result = cachedHandle.get();
        m_CachedHandles[resourceName] = std::move(cachedHandle);
        
        OLO_CORE_TRACE("ResourceHandleCache: Cached new handle for '{0}' (Handle: {1}, Type: {2})", 
                      resourceName, handle, static_cast<u32>(type));
        
        return result;
    }

    CachedHandle* ResourceHandleCache::GetCachedHandle(const std::string& resourceName)
    {
        m_TotalRequests.fetch_add(1, std::memory_order_relaxed);
        
        if (!m_CachingEnabled)
            return nullptr;

        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        auto it = m_CachedHandles.find(resourceName);
        if (it != m_CachedHandles.end() && it->second->IsValid)
        {
            it->second->Touch();
            m_CacheHits.fetch_add(1, std::memory_order_relaxed);
            return it->second.get();
        }
        
        return nullptr;
    }

    void ResourceHandleCache::InvalidateHandle(const std::string& resourceName)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        auto it = m_CachedHandles.find(resourceName);
        if (it != m_CachedHandles.end())
        {
            it->second->IsValid = false;
            m_InvalidatedHandles.insert(resourceName);
            
            OLO_CORE_TRACE("ResourceHandleCache: Invalidated handle for '{0}'", resourceName);
        }
    }

    void ResourceHandleCache::InvalidateHandlesByType(ShaderResourceType type)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        u32 invalidatedCount = 0;
        for (auto& [name, handle] : m_CachedHandles)
        {
            if (handle->Type == type && handle->IsValid)
            {
                handle->IsValid = false;
                m_InvalidatedHandles.insert(name);
                invalidatedCount++;
            }
        }
        
        OLO_CORE_TRACE("ResourceHandleCache: Invalidated {0} handles of type {1}", 
                      invalidatedCount, static_cast<u32>(type));
    }

    void ResourceHandleCache::RemoveHandle(const std::string& resourceName)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        auto it = m_CachedHandles.find(resourceName);
        if (it != m_CachedHandles.end())
        {
            m_CachedHandles.erase(it);
            m_InvalidatedHandles.erase(resourceName);
            
            OLO_CORE_TRACE("ResourceHandleCache: Removed handle for '{0}'", resourceName);
        }
    }

    bool ResourceHandleCache::AddHandleReference(const std::string& resourceName)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        auto it = m_CachedHandles.find(resourceName);
        if (it != m_CachedHandles.end())
        {
            it->second->AddRef();
            return true;
        }
        
        return false;
    }

    u32 ResourceHandleCache::RemoveHandleReference(const std::string& resourceName)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        auto it = m_CachedHandles.find(resourceName);
        if (it != m_CachedHandles.end())
        {
            u32 remainingRefs = it->second->RemoveRef();
            
            // Remove handle if no references remain and it's not pooled
            if (remainingRefs == 0 && !it->second->IsPooled)
            {
                m_CachedHandles.erase(it);
                m_InvalidatedHandles.erase(resourceName);
                
                OLO_CORE_TRACE("ResourceHandleCache: Removed unreferenced handle for '{0}'", resourceName);
            }
            
            return remainingRefs;
        }
        
        return 0;
    }

    void ResourceHandleCache::CleanupCache(u32 maxCacheSize, std::chrono::milliseconds maxAge)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        u32 initialSize = static_cast<u32>(m_CachedHandles.size());
        
        // First pass: Remove invalidated handles with no references
        auto invalidIt = m_InvalidatedHandles.begin();
        while (invalidIt != m_InvalidatedHandles.end())
        {
            auto handleIt = m_CachedHandles.find(*invalidIt);
            if (handleIt != m_CachedHandles.end() && 
                handleIt->second->GetRefCount() == 0 && 
                !handleIt->second->IsPooled)
            {
                m_CachedHandles.erase(handleIt);
                invalidIt = m_InvalidatedHandles.erase(invalidIt);
            }
            else
            {
                ++invalidIt;
            }
        }
        
        // Second pass: LRU eviction if still over size limit
        if (m_CachedHandles.size() > maxCacheSize)
        {
            EvictExpiredHandles(maxCacheSize, maxAge);
        }
        
        u32 finalSize = static_cast<u32>(m_CachedHandles.size());
        
        if (finalSize < initialSize)
        {
            OLO_CORE_TRACE("ResourceHandleCache: Cleaned up {0} handles ({1} -> {2})", 
                          initialSize - finalSize, initialSize, finalSize);
        }
        
        // Clean up handle pools
        if (m_UniformBufferPool)
            m_UniformBufferPool->CleanupOldResources(maxAge);
        if (m_StorageBufferPool)
            m_StorageBufferPool->CleanupOldResources(maxAge);
        if (m_Texture2DPool)
            m_Texture2DPool->CleanupOldResources(maxAge);
        if (m_TextureCubemapPool)
            m_TextureCubemapPool->CleanupOldResources(maxAge);
    }

    ResourceHandleCache::CacheStats ResourceHandleCache::GetStatistics() const
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        
        CacheStats stats;
        stats.TotalCachedHandles = static_cast<u32>(m_CachedHandles.size());
        stats.TotalRequests = m_TotalRequests.load(std::memory_order_relaxed);
        stats.CacheHits = m_CacheHits.load(std::memory_order_relaxed);
        
        if (stats.TotalRequests > 0)
        {
            stats.HitRate = static_cast<f64>(stats.CacheHits) / static_cast<f64>(stats.TotalRequests);
        }
        
        for (const auto& [name, handle] : m_CachedHandles)
        {
            if (handle->IsValid)
                stats.ValidHandles++;
            else
                stats.InvalidHandles++;
                
            if (handle->GetRefCount() > 0)
                stats.ReferencedHandles++;
                
            if (handle->IsPooled)
                stats.PooledHandles++;
                
            stats.TotalMemorySize += handle->MemorySize;
        }
        
        return stats;
    }

    void ResourceHandleCache::ResetStatistics()
    {
        m_TotalRequests.store(0, std::memory_order_relaxed);
        m_CacheHits.store(0, std::memory_order_relaxed);
        
        OLO_CORE_TRACE("ResourceHandleCache: Statistics reset");
    }

    void ResourceHandleCache::SetCachingEnabled(bool enabled)
    {
        m_CachingEnabled = enabled;
        
        if (!enabled)
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            m_CachedHandles.clear();
            m_InvalidatedHandles.clear();
            OLO_CORE_TRACE("ResourceHandleCache: Disabled and cleared cache");
        }
        else
        {
            OLO_CORE_TRACE("ResourceHandleCache: Enabled");
        }
    }

    void ResourceHandleCache::EvictExpiredHandles(u32 maxSize, std::chrono::milliseconds maxAge)
    {
        auto now = std::chrono::steady_clock::now();
        
        // Collect handles for potential eviction
        std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> candidates;
        
        for (const auto& [name, handle] : m_CachedHandles)
        {
            // Don't evict referenced or pooled handles
            if (handle->GetRefCount() == 0 && !handle->IsPooled)
            {
                candidates.emplace_back(name, handle->LastAccessed);
            }
        }
        
        // Sort by last accessed time (oldest first)
        std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b)
            {
                return a.second < b.second;
            });
        
        // Remove oldest handles until we're under the size limit
        u32 targetRemovalCount = static_cast<u32>(m_CachedHandles.size()) - maxSize;
        u32 removedCount = 0;
        
        for (const auto& [name, lastAccessed] : candidates)
        {
            if (removedCount >= targetRemovalCount)
                break;
                
            // Also check age limit
            if ((now - lastAccessed) > maxAge)
            {
                m_CachedHandles.erase(name);
                m_InvalidatedHandles.erase(name);
                removedCount++;
            }
        }
    }
}
