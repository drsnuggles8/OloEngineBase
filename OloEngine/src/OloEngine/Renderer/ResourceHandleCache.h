#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ShaderResourceTypes.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>

namespace OloEngine
{
    /**
     * @brief GPU resource handle with metadata for caching and pooling
     */
    struct CachedHandle
    {
        u32 Handle = 0;                                        // OpenGL resource handle
        ShaderResourceType Type = ShaderResourceType::None;   // Resource type
        std::string ResourceName;                              // Original resource name
        std::atomic<u32> ReferenceCount{0};                   // Reference counting for shared resources
        std::chrono::steady_clock::time_point LastAccessed;   // For LRU eviction
        std::chrono::steady_clock::time_point CreationTime;   // When handle was cached
        bool IsValid = true;                                   // Handle validity flag
        bool IsPooled = false;                                 // Whether this handle is from a pool
        sizet MemorySize = 0;                                  // Associated memory size

        CachedHandle() = default;
        CachedHandle(u32 handle, ShaderResourceType type, const std::string& name)
            : Handle(handle), Type(type), ResourceName(name)
        {
            auto now = std::chrono::steady_clock::now();
            LastAccessed = now;
            CreationTime = now;
        }

        // Increment reference count atomically
        void AddRef() { ReferenceCount.fetch_add(1, std::memory_order_relaxed); }
        
        // Decrement reference count atomically
        u32 RemoveRef() { return ReferenceCount.fetch_sub(1, std::memory_order_relaxed) - 1; }
        
        // Get current reference count
        u32 GetRefCount() const { return ReferenceCount.load(std::memory_order_relaxed); }
        
        // Update last accessed time
        void Touch() { LastAccessed = std::chrono::steady_clock::now(); }
    };

    /**
     * @brief Pool for temporary GPU handles of a specific type
     */
    template<typename ResourceType>
    class HandlePool
    {
    public:
        struct PooledResource
        {
            Ref<ResourceType> Resource;
            u32 Handle = 0;
            bool InUse = false;
            std::chrono::steady_clock::time_point LastUsed;
        };

    private:
        std::vector<PooledResource> m_Pool;
        ShaderResourceType m_ResourceType;
        u32 m_MaxPoolSize;
        std::function<Ref<ResourceType>()> m_Factory;
        mutable std::mutex m_Mutex;

    public:
        HandlePool(ShaderResourceType type, u32 maxSize, std::function<Ref<ResourceType>()> factory)
            : m_ResourceType(type), m_MaxPoolSize(maxSize), m_Factory(std::move(factory))
        {
        }

        /**
         * @brief Acquire a resource from the pool
         * @return Resource and its handle, or nullptr if pool is exhausted
         */
        std::pair<Ref<ResourceType>, u32> Acquire()
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            // Try to find an unused resource
            for (auto& pooled : m_Pool)
            {
                if (!pooled.InUse && pooled.Resource)
                {
                    pooled.InUse = true;
                    pooled.LastUsed = std::chrono::steady_clock::now();
                    return {pooled.Resource, pooled.Handle};
                }
            }
            
            // Create new resource if pool isn't full
            if (m_Pool.size() < m_MaxPoolSize && m_Factory)
            {
                auto resource = m_Factory();
                if (resource)
                {
                    PooledResource pooled;
                    pooled.Resource = resource;
                    pooled.InUse = true;
                    pooled.LastUsed = std::chrono::steady_clock::now();
                    
                    // Extract handle based on resource type
                    if constexpr (std::is_same_v<ResourceType, UniformBuffer>)
                    {
                        pooled.Handle = resource->GetRendererID();
                    }
                    else if constexpr (std::is_same_v<ResourceType, StorageBuffer>)
                    {
                        pooled.Handle = resource->GetRendererID();
                    }
                    else if constexpr (std::is_same_v<ResourceType, Texture2D>)
                    {
                        pooled.Handle = resource->GetRendererID();
                    }
                    else if constexpr (std::is_same_v<ResourceType, TextureCubemap>)
                    {
                        pooled.Handle = resource->GetRendererID();
                    }
                    
                    m_Pool.push_back(pooled);
                    return {resource, pooled.Handle};
                }
            }
            
            return {nullptr, 0};
        }

        /**
         * @brief Release a resource back to the pool
         * @param handle Handle of resource to release
         */
        void Release(u32 handle)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            for (auto& pooled : m_Pool)
            {
                if (pooled.Handle == handle && pooled.InUse)
                {
                    pooled.InUse = false;
                    pooled.LastUsed = std::chrono::steady_clock::now();
                    break;
                }
            }
        }

        /**
         * @brief Get pool statistics
         */
        struct PoolStats
        {
            u32 TotalResources = 0;
            u32 InUseResources = 0;
            u32 AvailableResources = 0;
            u32 MaxPoolSize = 0;
        };

        PoolStats GetStats() const
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            PoolStats stats;
            stats.TotalResources = static_cast<u32>(m_Pool.size());
            stats.MaxPoolSize = m_MaxPoolSize;
            
            for (const auto& pooled : m_Pool)
            {
                if (pooled.InUse)
                    stats.InUseResources++;
                else
                    stats.AvailableResources++;
            }
            
            return stats;
        }

        /**
         * @brief Clean up old unused resources
         * @param maxAge Maximum age for unused resources
         */
        void CleanupOldResources(std::chrono::milliseconds maxAge)
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            
            auto now = std::chrono::steady_clock::now();
            auto it = std::remove_if(m_Pool.begin(), m_Pool.end(),
                [now, maxAge](const PooledResource& pooled)
                {
                    return !pooled.InUse && 
                           (now - pooled.LastUsed) > maxAge;
                });
            
            m_Pool.erase(it, m_Pool.end());
        }
    };

    /**
     * @brief High-performance cache for frequently accessed GPU resource handles
     */
    class ResourceHandleCache
    {
    public:
        ResourceHandleCache();
        ~ResourceHandleCache();

        // No copy semantics
        ResourceHandleCache(const ResourceHandleCache&) = delete;
        ResourceHandleCache& operator=(const ResourceHandleCache&) = delete;

        // Move semantics
        ResourceHandleCache(ResourceHandleCache&&) = default;
        ResourceHandleCache& operator=(ResourceHandleCache&&) = default;

        /**
         * @brief Cache a GPU handle for fast access
         * @param resourceName Name of the resource
         * @param handle GPU handle (OpenGL ID)
         * @param type Resource type
         * @param memorySize Associated memory size
         * @return Cached handle reference
         */
        CachedHandle* CacheHandle(const std::string& resourceName, u32 handle, 
                                ShaderResourceType type, sizet memorySize = 0);

        /**
         * @brief Get a cached handle by resource name
         * @param resourceName Name of the resource
         * @return Cached handle if found and valid, nullptr otherwise
         */
        CachedHandle* GetCachedHandle(const std::string& resourceName);

        /**
         * @brief Invalidate a cached handle when resource changes
         * @param resourceName Name of the resource to invalidate
         */
        void InvalidateHandle(const std::string& resourceName);

        /**
         * @brief Invalidate handles by type (useful for bulk operations)
         * @param type Resource type to invalidate
         */
        void InvalidateHandlesByType(ShaderResourceType type);

        /**
         * @brief Remove a handle from cache
         * @param resourceName Name of the resource to remove
         */
        void RemoveHandle(const std::string& resourceName);

        /**
         * @brief Add reference to a shared handle
         * @param resourceName Name of the resource
         * @return True if handle exists and reference was added
         */
        bool AddHandleReference(const std::string& resourceName);

        /**
         * @brief Remove reference from a shared handle
         * @param resourceName Name of the resource
         * @return Remaining reference count, 0 if handle was removed
         */
        u32 RemoveHandleReference(const std::string& resourceName);

        /**
         * @brief Get handle pool for a specific resource type
         * @tparam T Resource type
         * @return Handle pool or nullptr if not available
         */
        template<typename T>
        HandlePool<T>* GetHandlePool();

        /**
         * @brief Create handle pool for a resource type
         * @tparam T Resource type
         * @param maxSize Maximum pool size
         * @param factory Factory function for creating resources
         */
        template<typename T>
        void CreateHandlePool(u32 maxSize, std::function<Ref<T>()> factory);

        /**
         * @brief Clean up cache based on LRU and reference counting
         * @param maxCacheSize Maximum number of cached handles
         * @param maxAge Maximum age for unreferenced handles
         */
        void CleanupCache(u32 maxCacheSize = 1024, 
                         std::chrono::milliseconds maxAge = std::chrono::minutes(5));

        /**
         * @brief Get cache statistics
         */
        struct CacheStats
        {
            u32 TotalCachedHandles = 0;
            u32 ValidHandles = 0;
            u32 InvalidHandles = 0;
            u32 ReferencedHandles = 0;
            u32 PooledHandles = 0;
            sizet TotalMemorySize = 0;
            f64 HitRate = 0.0;  // Cache hit rate since last reset
            u64 TotalRequests = 0;
            u64 CacheHits = 0;
        };

        CacheStats GetStatistics() const;

        /**
         * @brief Reset cache statistics
         */
        void ResetStatistics();

        /**
         * @brief Enable/disable handle caching
         * @param enabled Whether to enable caching
         */
        void SetCachingEnabled(bool enabled);

        /**
         * @brief Check if caching is enabled
         */
        bool IsCachingEnabled() const { return m_CachingEnabled; }

    private:
        std::unordered_map<std::string, std::unique_ptr<CachedHandle>> m_CachedHandles;
        std::unordered_set<std::string> m_InvalidatedHandles;
        
        // Handle pools for different resource types
        std::unique_ptr<HandlePool<UniformBuffer>> m_UniformBufferPool;
        std::unique_ptr<HandlePool<StorageBuffer>> m_StorageBufferPool;
        std::unique_ptr<HandlePool<Texture2D>> m_Texture2DPool;
        std::unique_ptr<HandlePool<TextureCubemap>> m_TextureCubemapPool;
        
        // Statistics
        mutable std::atomic<u64> m_TotalRequests{0};
        mutable std::atomic<u64> m_CacheHits{0};
        
        bool m_CachingEnabled = true;
        mutable std::mutex m_CacheMutex;

        /**
         * @brief Remove expired handles based on LRU policy
         * @param maxSize Maximum cache size
         * @param maxAge Maximum age for unreferenced handles
         */
        void EvictExpiredHandles(u32 maxSize, std::chrono::milliseconds maxAge);
    };

    // Template specializations for getting handle pools
    template<>
    inline HandlePool<UniformBuffer>* ResourceHandleCache::GetHandlePool<UniformBuffer>()
    {
        return m_UniformBufferPool.get();
    }

    template<>
    inline HandlePool<StorageBuffer>* ResourceHandleCache::GetHandlePool<StorageBuffer>()
    {
        return m_StorageBufferPool.get();
    }

    template<>
    inline HandlePool<Texture2D>* ResourceHandleCache::GetHandlePool<Texture2D>()
    {
        return m_Texture2DPool.get();
    }

    template<>
    inline HandlePool<TextureCubemap>* ResourceHandleCache::GetHandlePool<TextureCubemap>()
    {
        return m_TextureCubemapPool.get();
    }

    // Template specializations for creating handle pools
    template<>
    inline void ResourceHandleCache::CreateHandlePool<UniformBuffer>(u32 maxSize, std::function<Ref<UniformBuffer>()> factory)
    {
        m_UniformBufferPool = std::make_unique<HandlePool<UniformBuffer>>(
            ShaderResourceType::UniformBuffer, maxSize, std::move(factory));
    }

    template<>
    inline void ResourceHandleCache::CreateHandlePool<StorageBuffer>(u32 maxSize, std::function<Ref<StorageBuffer>()> factory)
    {
        m_StorageBufferPool = std::make_unique<HandlePool<StorageBuffer>>(
            ShaderResourceType::StorageBuffer, maxSize, std::move(factory));
    }

    template<>
    inline void ResourceHandleCache::CreateHandlePool<Texture2D>(u32 maxSize, std::function<Ref<Texture2D>()> factory)
    {
        m_Texture2DPool = std::make_unique<HandlePool<Texture2D>>(
            ShaderResourceType::Texture2D, maxSize, std::move(factory));
    }

    template<>
    inline void ResourceHandleCache::CreateHandlePool<TextureCubemap>(u32 maxSize, std::function<Ref<TextureCubemap>()> factory)
    {
        m_TextureCubemapPool = std::make_unique<HandlePool<TextureCubemap>>(
            ShaderResourceType::TextureCube, maxSize, std::move(factory));
    }
}
