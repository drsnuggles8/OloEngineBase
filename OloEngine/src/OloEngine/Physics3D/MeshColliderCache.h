#pragma once

#include "MeshCookingFactory.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetTypes.h"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <future>

namespace OloEngine {

	// Forward declarations
	class MeshColliderAsset;
	template<typename T> class Ref;
	struct CookingRequest;

	class MeshColliderCache
	{
	public:
		// Singleton access
		static MeshColliderCache& GetInstance();

		// Initialization
		void Initialize();
		void Shutdown();
		bool IsInitialized() const { return m_Initialized; }

		// Main cache interface
		const CachedColliderData& GetMeshData(Ref<MeshColliderAsset> colliderAsset);
		bool HasMeshData(Ref<MeshColliderAsset> colliderAsset) const;
		
		// Async cooking interface
		std::future<ECookingResult> CookMeshAsync(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type, bool invalidateOld = false);
		void ProcessCookingRequests();

		// Cache management
		void InvalidateCache(Ref<MeshColliderAsset> colliderAsset);
		void ClearCache();
		void PreloadCache(const std::vector<Ref<MeshColliderAsset>>& assets);

		// Cache statistics
		sizet GetCachedMeshCount() const;
		sizet GetMemoryUsage() const;
		f32 GetCacheHitRatio() const;

		// Settings
		void SetMaxCacheSize(sizet maxSizeBytes) { m_MaxCacheSize = maxSizeBytes; }
		sizet GetMaxCacheSize() const { return m_MaxCacheSize; }

		void SetMaxConcurrentCooks(u32 maxCooks) { m_MaxConcurrentCooks = maxCooks; }
		u32 GetMaxConcurrentCooks() const { return m_MaxConcurrentCooks; }

		// Debug info
		std::vector<AssetHandle> GetCachedAssets() const;
		CachedColliderData GetDebugMeshData(AssetHandle handle) const;

	private:
		// Cache management
		void EvictOldestEntries();
		bool ShouldEvictEntry(const CachedColliderData& data) const;
		sizet CalculateDataSize(const CachedColliderData& data) const;

		// Loading and cooking
		CachedColliderData LoadFromCache(Ref<MeshColliderAsset> colliderAsset);
		ECookingResult CookMeshImmediate(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType type, bool invalidateOld);

	private:
		// Singleton - private constructor/destructor
		MeshColliderCache();
		~MeshColliderCache();
		MeshColliderCache(const MeshColliderCache&) = delete;
		MeshColliderCache& operator=(const MeshColliderCache&) = delete;

		// Thread safety
		mutable std::mutex m_CacheMutex;
		std::mutex m_CookingMutex;

		// Cache storage
		std::unordered_map<AssetHandle, CachedColliderData> m_CachedData;
		sizet m_CurrentCacheSize = 0;
		sizet m_MaxCacheSize = 100 * 1024 * 1024; // 100MB default

		// Cooking system
		Ref<MeshCookingFactory> m_CookingFactory;
		std::vector<CookingRequest> m_CookingQueue;
		std::vector<std::future<void>> m_CookingTasks;
		u32 m_MaxConcurrentCooks = 4;

		// Statistics
		mutable std::atomic<sizet> m_CacheHits = 0;
		mutable std::atomic<sizet> m_CacheMisses = 0;

		// State
		bool m_Initialized = false;

		// Invalid/placeholder data
		CachedColliderData m_InvalidData;

		// Cache cleanup threshold
		static constexpr f32 CacheEvictionThreshold = 0.8f; // Start evicting at 80% capacity
		static constexpr sizet MinCacheEntryLifetimeMs = 5000; // 5 seconds minimum lifetime
	};

}