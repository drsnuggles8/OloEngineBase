#include "OloEngine/Physics3D/MeshColliderCache.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/MeshColliderAsset.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <thread>
#include <future>
#include <cmath>
#include <filesystem>

namespace OloEngine
{

    MeshColliderCache::MeshColliderCache()
    {
        m_CookingFactory = Ref<MeshCookingFactory>(new MeshCookingFactory());
    }

    MeshColliderCache::~MeshColliderCache()
    {
        if (m_IsInitialized.load(std::memory_order_acquire))
        {
            Shutdown();
        }
    }

    void MeshColliderCache::Initialize()
    {
        if (m_IsInitialized.load(std::memory_order_acquire))
        {
            OLO_CORE_WARN("MeshColliderCache already initialized");
            return;
        }

        // Initialize cooking factory with error handling
        try
        {
            m_CookingFactory->Initialize();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("MeshColliderCache: Failed to initialize cooking factory: {}", e.what());
            // Clean up all partial state to ensure no other methods attempt to use invalid factory
            m_CookingFactory->Shutdown();                                         // Clean up any partial initialization in factory
            m_CookingFactory = Ref<MeshCookingFactory>(new MeshCookingFactory()); // Recreate factory in clean state
            m_CachedData.clear();
            m_CookingQueue.clear();
            m_CookingTasks.clear();
            m_CurrentCacheSize = 0;
            // Keep m_IsInitialized as false and return early
            return;
        }

        // Reserve space for cache
        m_CachedData.reserve(s_InitialCacheReserve);

        m_IsInitialized.store(true, std::memory_order_release);
        OLO_CORE_INFO("MeshColliderCache initialized with max size: {}MB", m_MaxCacheSize.load() / s_BytesToMB);
    }

    void MeshColliderCache::Shutdown()
    {
        if (!m_IsInitialized.load(std::memory_order_acquire))
        {
            return;
        }

        // Wait for all cooking tasks to complete
        {
            std::lock_guard<std::mutex> lock(m_CookingMutex);
            for (auto& task : m_CookingTasks)
            {
                if (task.valid())
                {
                    task.wait();
                }
            }
            m_CookingTasks.clear();
            m_CookingQueue.clear();
        }

        // Clear cache
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            m_CachedData.clear();
            m_CurrentCacheSize = 0;
        }

        // Shutdown cooking factory
        m_CookingFactory->Shutdown();

        m_IsInitialized.store(false, std::memory_order_release);
        OLO_CORE_INFO("MeshColliderCache shutdown");
    }

    std::optional<CachedColliderData> MeshColliderCache::GetMeshData(Ref<MeshColliderAsset> colliderAsset)
    {
        if (!colliderAsset || !m_IsInitialized.load(std::memory_order_acquire))
        {
            m_CacheMisses.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        AssetHandle handle = colliderAsset->GetHandle();

        // Try to get from memory cache first
        if (auto cachedData = TryGetFromCache(handle))
        {
            return cachedData;
        }

        // Try to load from disk cache and add to memory cache
        if (auto loadedData = LoadAndCache(colliderAsset, handle))
        {
            return loadedData;
        }

        // Need to cook the mesh - determine primary and secondary types
        // Cook convex first (most common for dynamic bodies) and triangle async
        // TODO: Could be enhanced with caller hints or usage tracking
        EMeshColliderType primaryType = EMeshColliderType::Convex;
        EMeshColliderType secondaryType = EMeshColliderType::Triangle;

        return CookAndCache(colliderAsset, primaryType, secondaryType);
    }

    bool MeshColliderCache::HasMeshData(Ref<MeshColliderAsset> colliderAsset) const
    {
        if (!colliderAsset || !m_IsInitialized.load(std::memory_order_acquire))
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_CacheMutex);
        auto it = m_CachedData.find(colliderAsset->GetHandle());
        return it != m_CachedData.end() && it->second.m_IsValid;
    }

    std::future<ECookingResult> MeshColliderCache::CookMeshAsync(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type, bool invalidateOld)
    {
        std::lock_guard<std::mutex> lock(m_CookingMutex);

        CookingRequest request;
        request.m_ColliderAsset = colliderAsset;
        request.m_Type = type;
        request.m_InvalidateOld = invalidateOld;
        request.m_RequestTime = std::chrono::steady_clock::now();

        auto future = request.m_Promise.get_future();
        m_CookingQueue.push_back(std::move(request));

        return future;
    }

    void MeshColliderCache::ProcessCookingRequests()
    {
        std::lock_guard<std::mutex> lock(m_CookingMutex);

        // Clean up completed tasks
        m_CookingTasks.erase(
            std::remove_if(m_CookingTasks.begin(), m_CookingTasks.end(),
                           [](const std::future<void>& task)
                           {
                               return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                           }),
            m_CookingTasks.end());

        // Process new requests if we have capacity
        while (!m_CookingQueue.empty() && m_CookingTasks.size() < m_MaxConcurrentCooks.load())
        {
            CookingRequest request = std::move(m_CookingQueue.front());
            m_CookingQueue.pop_front();

            // Create async task that updates cache on completion
            auto task = std::async(std::launch::async, [this, req = std::move(request)]() mutable
                                   {
				ECookingResult result = m_CookingFactory->CookMeshType(req.m_ColliderAsset, req.m_Type, req.m_InvalidateOld);

				// If cooking succeeded, update the cache entry with the new data
				if (result == ECookingResult::Success && req.m_ColliderAsset)
				{
					AssetHandle handle = req.m_ColliderAsset->GetHandle();

					// Load the updated data from disk
					CachedColliderData updatedData = LoadFromCache(req.m_ColliderAsset);
					if (updatedData.m_IsValid)
					{
						// Update the in-memory cache
						std::lock_guard<std::mutex> lock(m_CacheMutex);
						auto it = m_CachedData.find(handle);
						if (it != m_CachedData.end())
						{
							// Update existing entry with new data
							sizet oldDataSize = CalculateDataSize(it->second);
							sizet newDataSize = CalculateDataSize(updatedData);

							// Update cache size tracking
							m_CurrentCacheSize = m_CurrentCacheSize - oldDataSize + newDataSize;

							// Replace with updated data
							it->second = updatedData;
						}
						else
						{
							// Add new entry if it doesn't exist (shouldn't normally happen)
							sizet dataSize = CalculateDataSize(updatedData);

							if (m_CurrentCacheSize + dataSize > m_MaxCacheSize.load() * s_CacheEvictionThreshold)
							{
								EvictOldestEntries();
							}

							m_CachedData[handle] = updatedData;
							m_CurrentCacheSize += dataSize;
						}
					}
				}

				req.m_Promise.set_value(result); });

            m_CookingTasks.push_back(std::move(task));
        }
    }

    CachedColliderData MeshColliderCache::LoadFromCache(Ref<MeshColliderAsset> colliderAsset)
    {
        CachedColliderData cachedData;

        if (!colliderAsset)
        {
            return cachedData;
        }

        // Try to load both simple and complex data from disk cache
        std::filesystem::path simpleCachePath = m_CookingFactory->GetCacheFilePath(colliderAsset, EMeshColliderType::Convex);
        std::filesystem::path complexCachePath = m_CookingFactory->GetCacheFilePath(colliderAsset, EMeshColliderType::Triangle);

        bool hasSimple = false;
        bool hasComplex = false;

        // Check if simple cache file exists with exception handling
        try
        {
            hasSimple = std::filesystem::exists(simpleCachePath);
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_WARN("Failed to check existence of simple cache file '{}': {}", simpleCachePath.string(), e.what());
            hasSimple = false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("Unexpected error checking simple cache file '{}': {}", simpleCachePath.string(), e.what());
            hasSimple = false;
        }

        // Check if complex cache file exists with exception handling
        try
        {
            hasComplex = std::filesystem::exists(complexCachePath);
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            OLO_CORE_WARN("Failed to check existence of complex cache file '{}': {}", complexCachePath.string(), e.what());
            hasComplex = false;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("Unexpected error checking complex cache file '{}': {}", complexCachePath.string(), e.what());
            hasComplex = false;
        }

        // Load simple collider data with exception handling
        if (hasSimple)
        {
            try
            {
                cachedData.m_SimpleColliderData = m_CookingFactory->DeserializeMeshCollider(simpleCachePath);
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                OLO_CORE_WARN("Failed to deserialize simple mesh collider from '{}': {}", simpleCachePath.string(), e.what());
                // Keep cachedData.m_SimpleColliderData in default (invalid) state
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("Unexpected error deserializing simple mesh collider from '{}': {}", simpleCachePath.string(), e.what());
                // Keep cachedData.m_SimpleColliderData in default (invalid) state
            }
        }

        // Load complex collider data with exception handling
        if (hasComplex)
        {
            try
            {
                cachedData.m_ComplexColliderData = m_CookingFactory->DeserializeMeshCollider(complexCachePath);
            }
            catch (const std::filesystem::filesystem_error& e)
            {
                OLO_CORE_WARN("Failed to deserialize complex mesh collider from '{}': {}", complexCachePath.string(), e.what());
                // Keep cachedData.m_ComplexColliderData in default (invalid) state
            }
            catch (const std::exception& e)
            {
                OLO_CORE_WARN("Unexpected error deserializing complex mesh collider from '{}': {}", complexCachePath.string(), e.what());
                // Keep cachedData.m_ComplexColliderData in default (invalid) state
            }
        }

        cachedData.m_IsValid = (hasSimple && cachedData.m_SimpleColliderData.m_IsValid) || (hasComplex && cachedData.m_ComplexColliderData.m_IsValid);

        if (cachedData.m_IsValid)
        {
            // Update last accessed time to reflect when cached data was loaded into memory
            cachedData.m_LastAccessed = std::chrono::system_clock::now();
        }

        return cachedData;
    }

    ECookingResult MeshColliderCache::CookMeshImmediate(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type, bool invalidateOld)
    {
        return m_CookingFactory->CookMeshType(colliderAsset, type, invalidateOld);
    }

    void MeshColliderCache::InvalidateCache(Ref<MeshColliderAsset> colliderAsset)
    {
        if (!colliderAsset)
        {
            return;
        }

        AssetHandle handle = colliderAsset->GetHandle();

        // Remove from memory cache
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            auto it = m_CachedData.find(handle);
            if (it != m_CachedData.end())
            {
                m_CurrentCacheSize -= CalculateDataSize(it->second);
                m_CachedData.erase(it);
            }
        }

        // Remove disk cache files
        std::filesystem::path simpleCachePath = m_CookingFactory->GetCacheFilePath(colliderAsset, EMeshColliderType::Convex);
        std::filesystem::path complexCachePath = m_CookingFactory->GetCacheFilePath(colliderAsset, EMeshColliderType::Triangle);

        try
        {
            if (std::filesystem::exists(simpleCachePath))
            {
                std::filesystem::remove(simpleCachePath);
            }
            if (std::filesystem::exists(complexCachePath))
            {
                std::filesystem::remove(complexCachePath);
            }
        }
        catch (const std::exception& e)
        {
            OLO_CORE_WARN("Failed to remove cache files for asset {}: {}", static_cast<u64>(handle), e.what());
        }

        OLO_CORE_TRACE("Invalidated cache for mesh collider asset {}", static_cast<u64>(handle));
    }

    void MeshColliderCache::ClearCache()
    {
        // Clear memory cache
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            m_CachedData.clear();
            m_CurrentCacheSize = 0;
        }

        // Clear disk cache
        m_CookingFactory->ClearCache();

        // Reset statistics with atomic operations
        m_CacheHits.store(0);
        m_CacheMisses.store(0);

        OLO_CORE_INFO("Mesh collider cache cleared");
    }

    void MeshColliderCache::PreloadCache(const std::vector<Ref<MeshColliderAsset>>& assets)
    {
        OLO_CORE_INFO("Preloading {} mesh collider assets", assets.size());

        for (const auto& asset : assets)
        {
            if (asset && !HasMeshData(asset))
            {
                // Queue for async cooking
                CookMeshAsync(asset, EMeshColliderType::Convex, false);
                CookMeshAsync(asset, EMeshColliderType::Triangle, false);
            }
        }
    }

    void MeshColliderCache::EvictOldestEntries()
    {
        // Simple LRU-like eviction - remove entries until we're under the threshold
        sizet targetSize = static_cast<sizet>(std::round(m_MaxCacheSize.load() * s_CacheEvictionThreshold * s_CacheEvictionTargetRatio));

        std::vector<std::pair<AssetHandle, std::chrono::system_clock::time_point>> entries;
        entries.reserve(m_CachedData.size());

        for (const auto& [handle, data] : m_CachedData)
        {
            entries.emplace_back(handle, data.m_LastAccessed);
        }

        // Sort by last accessed time (oldest first)
        std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs)
                  { return lhs.second < rhs.second; });

        // Remove oldest entries until we reach target size
        for (const auto& [handle, time] : entries)
        {
            if (m_CurrentCacheSize <= targetSize)
            {
                break;
            }

            auto it = m_CachedData.find(handle);
            if (it != m_CachedData.end())
            {
                m_CurrentCacheSize -= CalculateDataSize(it->second);
                m_CachedData.erase(it);
            }
        }

        OLO_CORE_TRACE("Evicted cache entries, new size: {}MB", m_CurrentCacheSize / (1024 * 1024));
    }

    bool MeshColliderCache::ShouldEvictEntry(const CachedColliderData& data) const
    {
        // Check if entry is old enough to be evicted
        auto now = std::chrono::system_clock::now();
        auto entryAge = std::chrono::duration_cast<std::chrono::milliseconds>(now - data.m_LastAccessed);

        return entryAge.count() > s_MinCacheEntryLifetimeMs;
    }

    sizet MeshColliderCache::CalculateDataSize(const CachedColliderData& data) const
    {
        sizet size = 0;

        // Calculate size of simple collider data
        for (const auto& submesh : data.m_SimpleColliderData.m_Submeshes)
        {
            size += submesh.m_ColliderData.size();
        }

        // Calculate size of complex collider data
        for (const auto& submesh : data.m_ComplexColliderData.m_Submeshes)
        {
            size += submesh.m_ColliderData.size();
        }

        // Add overhead for the data structures themselves
        size += sizeof(CachedColliderData);
        size += data.m_SimpleColliderData.m_Submeshes.size() * sizeof(SubmeshColliderData);
        size += data.m_ComplexColliderData.m_Submeshes.size() * sizeof(SubmeshColliderData);

        return size;
    }

    sizet MeshColliderCache::GetCachedMeshCount() const
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        return m_CachedData.size();
    }

    sizet MeshColliderCache::GetMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        return m_CurrentCacheSize;
    }

    f32 MeshColliderCache::GetCacheHitRatio() const
    {
        sizet hits = m_CacheHits.load();
        sizet misses = m_CacheMisses.load();
        sizet total = hits + misses;

        return total > 0 ? static_cast<f32>(hits) / static_cast<f32>(total) : 0.0f;
    }

    std::vector<AssetHandle> MeshColliderCache::GetCachedAssets() const
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);

        std::vector<AssetHandle> handles;
        handles.reserve(m_CachedData.size());

        for (const auto& [handle, data] : m_CachedData)
        {
            handles.push_back(handle);
        }

        return handles;
    }

    CachedColliderData MeshColliderCache::GetDebugMeshData(AssetHandle handle) const
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);

        auto it = m_CachedData.find(handle);
        if (it != m_CachedData.end())
        {
            return it->second;
        }

        return m_InvalidData;
    }

    // Helper methods for GetMeshData refactoring
    std::optional<CachedColliderData> MeshColliderCache::TryGetFromCache(AssetHandle handle)
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        auto it = m_CachedData.find(handle);
        if (it != m_CachedData.end() && it->second.m_IsValid)
        {
            m_CacheHits.fetch_add(1, std::memory_order_relaxed);
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<CachedColliderData> MeshColliderCache::LoadAndCache(Ref<MeshColliderAsset> colliderAsset, AssetHandle handle)
    {
        // Try to load from disk cache
        CachedColliderData loadedData = LoadFromCache(colliderAsset);
        if (!loadedData.m_IsValid)
        {
            return std::nullopt;
        }

        // Add to memory cache with proper eviction management
        CachedColliderData cachedResult;
        {
            std::lock_guard<std::mutex> lock(m_CacheMutex);
            sizet dataSize = CalculateDataSize(loadedData);

            // Check if we need to evict entries
            if (m_CurrentCacheSize + dataSize > m_MaxCacheSize.load() * s_CacheEvictionThreshold)
            {
                EvictOldestEntries();
            }

            m_CachedData[handle] = std::move(loadedData);
            m_CurrentCacheSize += dataSize;
            cachedResult = m_CachedData[handle];
        }

        m_CacheHits.fetch_add(1, std::memory_order_relaxed);
        return cachedResult;
    }

    std::optional<CachedColliderData> MeshColliderCache::CookAndCache(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType primaryType, EMeshColliderType secondaryType)
    {
        AssetHandle handle = colliderAsset->GetHandle();

        // Increment cache miss counter for cooking attempts
        m_CacheMisses.fetch_add(1, std::memory_order_relaxed);

        // Cook the primary type synchronously for immediate use
        ECookingResult primaryResult = CookMeshImmediate(colliderAsset, primaryType, false);

        // Queue the secondary type for asynchronous cooking
        std::future<ECookingResult> secondaryFuture = CookMeshAsync(colliderAsset, secondaryType, false);

        // If primary cooking succeeded, try loading the cache
        if (primaryResult == ECookingResult::Success)
        {
            CachedColliderData loadedData = LoadFromCache(colliderAsset);
            if (loadedData.m_IsValid)
            {
                CachedColliderData cachedResult;
                {
                    std::lock_guard<std::mutex> lock(m_CacheMutex);
                    sizet dataSize = CalculateDataSize(loadedData);

                    if (m_CurrentCacheSize + dataSize > m_MaxCacheSize.load() * s_CacheEvictionThreshold)
                    {
                        EvictOldestEntries();
                    }

                    m_CachedData[handle] = std::move(loadedData);
                    m_CurrentCacheSize += dataSize;
                    cachedResult = m_CachedData[handle];
                }

                // Note: Secondary cooking will update the cache asynchronously when complete
                return cachedResult;
            }
        }

        // If primary failed, wait for secondary to complete and try again
        ECookingResult secondaryResult = ECookingResult::Failed;

        // Use timed wait to avoid indefinite blocking
        constexpr auto timeout = std::chrono::seconds(5);
        auto waitStatus = secondaryFuture.wait_for(timeout);

        if (waitStatus == std::future_status::ready)
        {
            secondaryResult = secondaryFuture.get();
        }
        else
        {
            OLO_CORE_WARN("Secondary mesh cooking timed out after {} seconds for asset {}",
                          timeout.count(), static_cast<u64>(handle));
            // secondaryResult remains ECookingResult::Failed
        }

        if (secondaryResult == ECookingResult::Success)
        {
            CachedColliderData loadedData = LoadFromCache(colliderAsset);
            if (loadedData.m_IsValid)
            {
                CachedColliderData cachedResult;
                {
                    std::lock_guard<std::mutex> lock(m_CacheMutex);
                    sizet dataSize = CalculateDataSize(loadedData);

                    if (m_CurrentCacheSize + dataSize > m_MaxCacheSize.load() * s_CacheEvictionThreshold)
                    {
                        EvictOldestEntries();
                    }

                    m_CachedData[handle] = std::move(loadedData);
                    m_CurrentCacheSize += dataSize;
                    cachedResult = m_CachedData[handle];
                }

                return cachedResult;
            }
        }

        // Return nullopt if everything failed
        return std::nullopt;
    }

} // namespace OloEngine
