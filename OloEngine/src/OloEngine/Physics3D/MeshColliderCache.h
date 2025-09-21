#pragma once

#include "MeshCookingFactory.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/MeshColliderAsset.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <future>

namespace OloEngine {

	// Forward declarations
	template<typename T> class Ref;

	// Cooking request structure
	struct CookingRequest
	{
		Ref<MeshColliderAsset> m_ColliderAsset;
		EMeshColliderType m_Type;
		bool m_InvalidateOld = false;
		std::promise<ECookingResult> m_Promise;
		std::chrono::steady_clock::time_point m_RequestTime;
	};

	class MeshColliderCache
	{
	public:
		// Singleton access - thread-safe Meyers singleton
		static inline MeshColliderCache& GetInstance()
		{
			static MeshColliderCache instance;
			return instance;
		}

		// Initialization
		void Initialize();
		void Shutdown();
		bool IsInitialized() const { return m_IsInitialized; }

		// Main cache interface
		/// @brief Safely get cached mesh data for an asset
		/// @param colliderAsset The mesh collider asset to get data for
		/// @return Optional reference to cached data, or std::nullopt if not available
		/// @note Always check the returned optional before accessing. Use HasMeshData() to test availability.
		std::optional<CachedColliderData> GetMeshData(Ref<MeshColliderAsset> colliderAsset);
		
		/// @brief Check if cached mesh data is available for an asset
		/// @param colliderAsset The mesh collider asset to check
		/// @return true if valid cached data is available, false otherwise
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
		void SetMaxCacheSize(sizet maxSizeBytes) { m_MaxCacheSize.store(maxSizeBytes, std::memory_order_relaxed); }
		sizet GetMaxCacheSize() const { return m_MaxCacheSize.load(std::memory_order_relaxed); }

		void SetMaxConcurrentCooks(u32 maxCooks) { m_MaxConcurrentCooks.store(maxCooks, std::memory_order_relaxed); }
		u32 GetMaxConcurrentCooks() const { return m_MaxConcurrentCooks.load(std::memory_order_relaxed); }

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

		// Helper methods for GetMeshData refactoring
		std::optional<CachedColliderData> TryGetFromCache(AssetHandle handle);
		std::optional<CachedColliderData> LoadAndCache(Ref<MeshColliderAsset> colliderAsset, AssetHandle handle);
		std::optional<CachedColliderData> CookAndCache(Ref<MeshColliderAsset> colliderAsset, EMeshColliderType primaryType, EMeshColliderType secondaryType);

	private:
		// Singleton - private constructor/destructor
		MeshColliderCache();
		~MeshColliderCache();
		
		// Deleted copy and move operations to prevent duplication
		MeshColliderCache(const MeshColliderCache&) = delete;
		MeshColliderCache& operator=(const MeshColliderCache&) = delete;
		MeshColliderCache(MeshColliderCache&&) = delete;
		MeshColliderCache& operator=(MeshColliderCache&&) = delete;

		// Thread safety
		mutable std::mutex m_CacheMutex;
		std::mutex m_CookingMutex;

		// Cache storage
		std::unordered_map<AssetHandle, CachedColliderData> m_CachedData;
		sizet m_CurrentCacheSize = 0;
		std::atomic<sizet> m_MaxCacheSize = 100 * 1024 * 1024; // 100MB default

		// Cooking system
		Ref<MeshCookingFactory> m_CookingFactory;
		std::deque<CookingRequest> m_CookingQueue;
		std::vector<std::future<void>> m_CookingTasks;
		std::atomic<u32> m_MaxConcurrentCooks = 4;

		// Statistics
		mutable std::atomic<sizet> m_CacheHits = 0;
		mutable std::atomic<sizet> m_CacheMisses = 0;

		// State
		std::atomic<bool> m_IsInitialized = false;

		// Invalid/placeholder data
		CachedColliderData m_InvalidData;

		// Cache cleanup threshold
		static constexpr f32 s_CacheEvictionThreshold = 0.8f; // Start evicting at 80% capacity
		static constexpr f32 s_CacheEvictionTargetRatio = 0.7f; // Target cache size after eviction (70% of threshold)
		static constexpr sizet s_MinCacheEntryLifetimeMs = 5000; // 5 seconds minimum lifetime
		
		// Cache initialization constants
		static constexpr sizet s_InitialCacheReserve = 1024; // Initial cache container reserve size
		static constexpr sizet s_BytesToMB = 1024 * 1024; // Bytes to megabytes conversion factor
	};

}