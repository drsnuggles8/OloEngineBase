#include "OloEngine/Physics3D/MeshColliderCache.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/AssetManager.h"

#include <chrono>
#include <algorithm>
#include <thread>

namespace OloEngine {

	MeshColliderCache& MeshColliderCache::GetInstance()
	{
		static MeshColliderCache instance;
		return instance;
	}

	MeshColliderCache::MeshColliderCache()
	{
		m_CookingFactory = Ref<MeshCookingFactory>(new MeshCookingFactory());
	}

	MeshColliderCache::~MeshColliderCache()
	{
		if (m_Initialized)
		{
			Shutdown();
		}
	}

	void MeshColliderCache::Initialize()
	{
		if (m_Initialized)
		{
			OLO_CORE_WARN("MeshColliderCache already initialized");
			return;
		}

		// Initialize cooking factory
		m_CookingFactory->Initialize();

		// Reserve space for cache
		m_CachedData.reserve(1024);

		m_Initialized = true;
		OLO_CORE_INFO("MeshColliderCache initialized with max size: {}MB", m_MaxCacheSize / (1024 * 1024));
	}

	void MeshColliderCache::Shutdown()
	{
		if (!m_Initialized)
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

		m_Initialized = false;
		OLO_CORE_INFO("MeshColliderCache shutdown");
	}

	const CachedColliderData& MeshColliderCache::GetMeshData(Ref<MeshColliderAsset> colliderAsset)
	{
		if (!colliderAsset || !m_Initialized)
		{
			m_CacheMisses++;
			return m_InvalidData;
		}

		AssetHandle handle = colliderAsset->GetHandle();

		// Try to get from cache first
		{
			std::lock_guard<std::mutex> lock(m_CacheMutex);
			auto it = m_CachedData.find(handle);
			if (it != m_CachedData.end() && it->second.m_IsValid)
			{
				m_CacheHits++;
				return it->second;
			}
		}

		// Cache miss - try to load from disk cache
		CachedColliderData loadedData = LoadFromCache(colliderAsset);
		if (loadedData.m_IsValid)
		{
			// Add to memory cache
			std::lock_guard<std::mutex> lock(m_CacheMutex);
			sizet dataSize = CalculateDataSize(loadedData);
			
			// Check if we need to evict entries
			if (m_CurrentCacheSize + dataSize > m_MaxCacheSize * CacheEvictionThreshold)
			{
				EvictOldestEntries();
			}

			m_CachedData[handle] = loadedData;
			m_CurrentCacheSize += dataSize;
			
			m_CacheHits++;
			return m_CachedData[handle];
		}

		// Need to cook the mesh
		m_CacheMisses++;
		
		// Cook both simple and complex versions synchronously for immediate use
		ECookingResult simpleResult = CookMeshImmediate(colliderAsset, EMeshColliderType::Convex, false);
		ECookingResult complexResult = CookMeshImmediate(colliderAsset, EMeshColliderType::Triangle, false);

		if (simpleResult == ECookingResult::Success || complexResult == ECookingResult::Success)
		{
			// Try loading again after cooking
			loadedData = LoadFromCache(colliderAsset);
			if (loadedData.m_IsValid)
			{
				std::lock_guard<std::mutex> lock(m_CacheMutex);
				sizet dataSize = CalculateDataSize(loadedData);
				
				if (m_CurrentCacheSize + dataSize > m_MaxCacheSize * CacheEvictionThreshold)
				{
					EvictOldestEntries();
				}

				m_CachedData[handle] = loadedData;
				m_CurrentCacheSize += dataSize;
				
				return m_CachedData[handle];
			}
		}

		// Return invalid data if everything failed
		return m_InvalidData;
	}

	bool MeshColliderCache::HasMeshData(Ref<MeshColliderAsset> colliderAsset) const
	{
		if (!colliderAsset || !m_Initialized)
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
				[](const std::future<void>& task) {
					return task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
				}),
			m_CookingTasks.end()
		);

		// Process new requests if we have capacity
		while (!m_CookingQueue.empty() && m_CookingTasks.size() < m_MaxConcurrentCooks)
		{
			CookingRequest request = std::move(m_CookingQueue.back());
			m_CookingQueue.pop_back();

			// Create async task
			auto task = std::async(std::launch::async, [this, req = std::move(request)]() mutable {
				ECookingResult result = m_CookingFactory->CookMeshType(req.m_ColliderAsset, req.m_Type, req.m_InvalidateOld);
				req.m_Promise.set_value(result);
			});

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

		bool hasSimple = std::filesystem::exists(simpleCachePath);
		bool hasComplex = std::filesystem::exists(complexCachePath);

		if (hasSimple)
		{
			cachedData.m_SimpleColliderData = m_CookingFactory->DeserializeMeshCollider(simpleCachePath);
		}

		if (hasComplex)
		{
			cachedData.m_ComplexColliderData = m_CookingFactory->DeserializeMeshCollider(complexCachePath);
		}

		cachedData.m_IsValid = (hasSimple && cachedData.m_SimpleColliderData.m_IsValid) || 
							   (hasComplex && cachedData.m_ComplexColliderData.m_IsValid);

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

		// Reset statistics
		m_CacheHits = 0;
		m_CacheMisses = 0;

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
		sizet targetSize = static_cast<sizet>(m_MaxCacheSize * CacheEvictionThreshold * 0.7f); // Evict to 70% of threshold

		std::vector<std::pair<AssetHandle, std::chrono::system_clock::time_point>> entries;
		entries.reserve(m_CachedData.size());

		for (const auto& [handle, data] : m_CachedData)
		{
			entries.emplace_back(handle, data.m_LastAccessed);
		}

		// Sort by last accessed time (oldest first)
		std::sort(entries.begin(), entries.end(),
			[](const auto& a, const auto& b) {
				return a.second < b.second;
			});

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
		
		return entryAge.count() > MinCacheEntryLifetimeMs;
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

}