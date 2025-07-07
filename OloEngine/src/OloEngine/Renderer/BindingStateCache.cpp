#include "BindingStateCache.h"
#include "UniformBufferRegistry.h"
#include "ShaderResourceBinding.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Profiler.h"
#include <algorithm>
#include <functional>

namespace OloEngine
{
    // Global cache instance
    static BindingStateCache* s_GlobalCache = nullptr;

    // Hash function for combining values
    constexpr u64 CombineHash(u64 a, u64 b)
    {
        return a ^ (b + 0x9e3779b9 + (a << 6) + (a >> 2));
    }

    // CachedBindingState Implementation
    u64 CachedBindingState::CalculateHash() const
    {
        u64 hash = ResourceHandle;
        hash = CombineHash(hash, BindingPoint);
        hash = CombineHash(hash, static_cast<u64>(Target));
        hash = CombineHash(hash, static_cast<u64>(Offset));
        hash = CombineHash(hash, static_cast<u64>(Size));
        return hash;
    }

    bool CachedBindingState::MatchesState(const CachedBindingState& other) const
    {
        return ResourceHandle == other.ResourceHandle &&
               BindingPoint == other.BindingPoint &&
               Target == other.Target &&
               Offset == other.Offset &&
               Size == other.Size &&
               Set == other.Set;
    }

    void CachedBindingState::UpdateAccess()
    {
        LastAccessed = std::chrono::steady_clock::now();
        AccessCount++;
    }

    void CachedBindingState::MarkBound(u32 frameNumber)
    {
        IsActive = true;
        IsDirty = false;
        LastBoundFrame = frameNumber;
        BindCount++;
        
        auto now = std::chrono::steady_clock::now();
        LastAccessed = now;
        
        if (BindCount == 1)
        {
            FirstBound = now;
        }
        
        StateHash = CalculateHash();
    }

    bool CachedBindingState::IsStale(std::chrono::milliseconds maxAge) const
    {
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - LastAccessed);
        return age > maxAge;
    }

    // GlobalBindingState Implementation
    void GlobalBindingState::Invalidate()
    {
        UniformBufferBindings.clear();
        StorageBufferBindings.clear();
        TextureBindings.clear();
        ImageBindings.clear();
        IsValid = false;
    }

    bool GlobalBindingState::ValidateAgainstOpenGL()
    {
        OLO_PROFILE_FUNCTION();
        
        // Sample validation - check a few key bindings
        GLint currentUBO = 0;
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &currentUBO);
        
        GLint currentProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
        
        GLint currentVAO = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &currentVAO);
        
        bool isValid = (CurrentProgram == static_cast<u32>(currentProgram)) &&
                       (CurrentVAO == static_cast<u32>(currentVAO));
        
        if (!isValid)
        {
            OLO_CORE_WARN("Global binding state validation failed - cache is stale");
            Invalidate();
        }
        
        return isValid;
    }

    void GlobalBindingState::UpdateBinding(GLenum target, u32 bindingPoint, u32 handle)
    {
        switch (target)
        {
            case GL_UNIFORM_BUFFER:
                UniformBufferBindings[bindingPoint] = handle;
                break;
            case GL_SHADER_STORAGE_BUFFER:
                StorageBufferBindings[bindingPoint] = handle;
                break;
            case GL_TEXTURE_2D:
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_CUBE_MAP:
                TextureBindings[bindingPoint] = handle;
                break;
            case GL_TEXTURE_2D + 1000: // Image bindings (using offset to distinguish)
                ImageBindings[bindingPoint] = handle;
                break;
        }
    }

    // BindingStateCache Implementation
    BindingStateCache::BindingStateCache() = default;

    BindingStateCache::~BindingStateCache()
    {
        if (m_IsInitialized)
        {
            Shutdown();
        }
    }

    bool BindingStateCache::Initialize(CachePolicy policy, InvalidationStrategy strategy)
    {
        OLO_PROFILE_FUNCTION();
        
        if (m_IsInitialized)
        {
            OLO_CORE_WARN("BindingStateCache: Already initialized");
            return true;
        }

        m_CachePolicy = policy;
        m_InvalidationStrategy = strategy;
        
        // Reserve space for common binding points
        m_BindingCache.reserve(128);
        
        // Initialize global state tracking
        m_GlobalState.Invalidate();
        
        m_IsInitialized = true;
        
        OLO_CORE_INFO("BindingStateCache: Initialized with {} policy and {} invalidation", 
                      static_cast<int>(policy), static_cast<int>(strategy));
        
        return true;
    }

    void BindingStateCache::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        
        if (!m_IsInitialized)
        {
            return;
        }

        // Log final statistics
        m_Statistics.UpdateHitRate();
        OLO_CORE_INFO("BindingStateCache: Shutdown - Hit Rate: {:.1f}%, Time Saved: {:.2f}ms, Cache Size: {}", 
                      m_Statistics.HitRate, m_Statistics.TimeSaved, m_BindingCache.size());
        
        m_BindingCache.clear();
        m_GlobalState.Invalidate();
        m_Statistics.Reset();
        
        m_IsInitialized = false;
    }

    bool BindingStateCache::IsBindingRedundant(GLenum target, u32 bindingPoint, u32 resourceHandle, 
                                             GLintptr offset, GLsizeiptr size)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!m_IsInitialized || m_CachePolicy == CachePolicy::Minimal)
        {
            return false;
        }

        u64 key = GenerateCacheKey(target, bindingPoint);
        auto it = m_BindingCache.find(key);
        
        if (it == m_BindingCache.end())
        {
            UpdateStatistics(false);
            return false; // Not cached, not redundant
        }

        CachedBindingState& cached = it->second;
        cached.UpdateAccess();

        // Check if the binding parameters match
        bool isRedundant = cached.ResourceHandle == resourceHandle &&
                          cached.Offset == offset &&
                          cached.Size == size &&
                          cached.IsActive &&
                          !cached.IsDirty;

        if (isRedundant)
        {
            UpdateStatistics(true, 0.05f); // Assume 0.05ms saved per redundant call
            OLO_CORE_TRACE("BindingStateCache: Redundant binding avoided - target: {}, point: {}, handle: {}", 
                           target, bindingPoint, resourceHandle);
        }
        else
        {
            UpdateStatistics(false);
        }

        return isRedundant;
    }

    void BindingStateCache::RecordBinding(GLenum target, u32 bindingPoint, u32 resourceHandle, 
                                        ShaderResourceType resourceType, GLintptr offset, 
                                        GLsizeiptr size, u32 frameNumber)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!m_IsInitialized || !ShouldCache(target, resourceType))
        {
            return;
        }

        u64 key = GenerateCacheKey(target, bindingPoint);
        
        // Find or create cache entry
        auto [it, inserted] = m_BindingCache.try_emplace(key, resourceHandle, bindingPoint, target, resourceType);
        CachedBindingState& cached = it->second;

        // Update binding state
        cached.ResourceHandle = resourceHandle;
        cached.Offset = offset;
        cached.Size = size;
        cached.Type = resourceType;
        cached.MarkBound(frameNumber > 0 ? frameNumber : m_CurrentFrame);

        // Update global state
        m_GlobalState.UpdateBinding(target, bindingPoint, resourceHandle);

        OLO_CORE_TRACE("BindingStateCache: Recorded binding - target: {}, point: {}, handle: {}, frame: {}", 
                       target, bindingPoint, resourceHandle, cached.LastBoundFrame);
    }

    u32 BindingStateCache::ApplyRegistryBindings(const UniformBufferRegistry& registry, bool forceRebind)
    {
        OLO_PROFILE_FUNCTION();
        
        u32 appliedCount = 0;
        
        // Get current frame number
        u32 currentFrame = m_CurrentFrame;
        
        // Apply each binding from the registry
        const auto& bindings = registry.GetAllBindings();
        for (const auto& [bindingPoint, binding] : bindings)
        {
            if (!binding.IsValid())
            {
                continue;
            }

            bool shouldBind = forceRebind || 
                            !IsBindingRedundant(GL_UNIFORM_BUFFER, bindingPoint, 
                                              binding.GetBufferHandle(), 
                                              binding.GetOffset(), 
                                              binding.GetSize());

            if (shouldBind)
            {
                // Apply the binding
                glBindBufferRange(GL_UNIFORM_BUFFER, bindingPoint, 
                                binding.GetBufferHandle(), 
                                binding.GetOffset(), 
                                binding.GetSize());

                // Record in cache
                RecordBinding(GL_UNIFORM_BUFFER, bindingPoint, binding.GetBufferHandle(), 
                            binding.GetResourceType(), binding.GetOffset(), 
                            binding.GetSize(), currentFrame);
                
                appliedCount++;
            }
        }

        return appliedCount;
    }

    void BindingStateCache::InvalidateBinding(GLenum target, u32 bindingPoint)
    {
        u64 key = GenerateCacheKey(target, bindingPoint);
        auto it = m_BindingCache.find(key);
        
        if (it != m_BindingCache.end())
        {
            it->second.IsDirty = true;
            it->second.IsActive = false;
            OLO_CORE_TRACE("BindingStateCache: Invalidated binding - target: {}, point: {}", target, bindingPoint);
        }

        m_Statistics.CacheInvalidations++;
    }

    void BindingStateCache::InvalidateBindingsOfType(GLenum target)
    {
        u32 invalidatedCount = 0;
        
        for (auto& [key, cached] : m_BindingCache)
        {
            if (cached.Target == target)
            {
                cached.IsDirty = true;
                cached.IsActive = false;
                invalidatedCount++;
            }
        }

        m_Statistics.CacheInvalidations += invalidatedCount;
        OLO_CORE_TRACE("BindingStateCache: Invalidated {} bindings of type {}", invalidatedCount, target);
    }

    void BindingStateCache::InvalidateAllBindings()
    {
        OLO_PROFILE_FUNCTION();
        
        for (auto& [key, cached] : m_BindingCache)
        {
            cached.IsDirty = true;
            cached.IsActive = false;
        }

        m_GlobalState.Invalidate();
        m_Statistics.CacheInvalidations++;
        
        OLO_CORE_TRACE("BindingStateCache: Invalidated all bindings");
    }

    void BindingStateCache::SetCurrentFrame(u32 frameNumber)
    {
        m_CurrentFrame = frameNumber;
        
        // Perform frame-based invalidation
        if (m_InvalidationStrategy == InvalidationStrategy::FrameBased)
        {
            // Invalidate bindings older than a certain number of frames
            const u32 maxFrameAge = 5;
            u32 invalidatedCount = 0;
            
            for (auto& [key, cached] : m_BindingCache)
            {
                if (frameNumber > cached.LastBoundFrame + maxFrameAge)
                {
                    cached.IsDirty = true;
                    invalidatedCount++;
                }
            }
            
            if (invalidatedCount > 0)
            {
                OLO_CORE_TRACE("BindingStateCache: Frame-based invalidation removed {} old bindings", invalidatedCount);
            }
        }

        // Perform automatic cleanup if enabled
        if (m_AutoCleanupEnabled && (frameNumber - m_LastCleanupFrame) >= m_CleanupInterval)
        {
            PerformAutomaticCleanup();
            m_LastCleanupFrame = frameNumber;
        }
    }

    u32 BindingStateCache::CleanupStaleBindings(std::chrono::milliseconds maxAge)
    {
        OLO_PROFILE_FUNCTION();
        
        u32 removedCount = 0;
        
        for (auto it = m_BindingCache.begin(); it != m_BindingCache.end();)
        {
            if (it->second.IsStale(maxAge))
            {
                it = m_BindingCache.erase(it);
                removedCount++;
            }
            else
            {
                ++it;
            }
        }

        m_Statistics.StaleBindingsRemoved += removedCount;
        
        if (removedCount > 0)
        {
            OLO_CORE_TRACE("BindingStateCache: Cleaned up {} stale bindings", removedCount);
        }
        
        return removedCount;
    }

    bool BindingStateCache::ValidateCache(bool fullValidation)
    {
        OLO_PROFILE_FUNCTION();
        
        if (!m_ValidationEnabled)
        {
            return true;
        }

        bool isValid = m_GlobalState.ValidateAgainstOpenGL();
        m_Statistics.StateValidations++;

        if (fullValidation)
        {
            // Validate individual bindings against OpenGL state
            u32 mismatchCount = 0;
            
            for (auto& [key, cached] : m_BindingCache)
            {
                if (!cached.IsActive)
                {
                    continue;
                }

                u32 actualHandle = GetActualOpenGLBinding(cached.Target, cached.BindingPoint);
                if (actualHandle != cached.ResourceHandle)
                {
                    cached.IsDirty = true;
                    cached.IsActive = false;
                    mismatchCount++;
                }
            }
            
            if (mismatchCount > 0)
            {
                OLO_CORE_WARN("BindingStateCache: Full validation found {} mismatches", mismatchCount);
                isValid = false;
            }
        }

        return isValid;
    }

    void BindingStateCache::SynchronizeWithOpenGL()
    {
        OLO_PROFILE_FUNCTION();
        
        // Clear cache and rebuild from actual OpenGL state
        m_BindingCache.clear();
        m_GlobalState.Invalidate();
        
        // Query current OpenGL state
        GLint currentProgram = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
        m_GlobalState.CurrentProgram = static_cast<u32>(currentProgram);
        
        GLint currentVAO = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &currentVAO);
        m_GlobalState.CurrentVAO = static_cast<u32>(currentVAO);
        
        m_GlobalState.IsValid = true;
        
        OLO_CORE_INFO("BindingStateCache: Synchronized with OpenGL state");
    }

    const CachedBindingState* BindingStateCache::GetCachedBinding(GLenum target, u32 bindingPoint) const
    {
        u64 key = GenerateCacheKey(target, bindingPoint);
        auto it = m_BindingCache.find(key);
        return (it != m_BindingCache.end()) ? &it->second : nullptr;
    }

    std::vector<const CachedBindingState*> BindingStateCache::GetCachedBindingsOfType(GLenum target) const
    {
        std::vector<const CachedBindingState*> result;
        
        for (const auto& [key, cached] : m_BindingCache)
        {
            if (cached.Target == target)
            {
                result.push_back(&cached);
            }
        }
        
        return result;
    }

    BindingStateCache::CacheInfo BindingStateCache::GetCacheInfo() const
    {
        CacheInfo info;
        info.TotalBindings = static_cast<u32>(m_BindingCache.size());
        
        for (const auto& [key, cached] : m_BindingCache)
        {
            if (cached.IsActive)
            {
                info.ActiveBindings++;
            }
            if (cached.IsStale())
            {
                info.StaleBindings++;
            }
        }
        
        info.MemoryUsage = m_BindingCache.size() * sizeof(CachedBindingState);
        
        return info;
    }

    // Private methods
    u64 BindingStateCache::GenerateCacheKey(GLenum target, u32 bindingPoint) const
    {
        return (static_cast<u64>(target) << 32) | static_cast<u64>(bindingPoint);
    }

    void BindingStateCache::UpdateStatistics(bool wasHit, f32 bindTime) const
    {
        if (wasHit)
        {
            m_Statistics.TotalCacheHits++;
            m_Statistics.TimeSaved += bindTime;
        }
        else
        {
            m_Statistics.TotalCacheMisses++;
        }
        
        m_Statistics.UpdateHitRate();
    }

    bool BindingStateCache::ShouldCache(GLenum target, ShaderResourceType resourceType) const
    {
        switch (m_CachePolicy)
        {
            case CachePolicy::Minimal:
                return false;
            
            case CachePolicy::Conservative:
                return target == GL_UNIFORM_BUFFER || target == GL_SHADER_STORAGE_BUFFER;
            
            case CachePolicy::Balanced:
                return resourceType != ShaderResourceType::None;
            
            case CachePolicy::Aggressive:
                return true;
        }
        
        return true;
    }

    bool BindingStateCache::ShouldValidate() const
    {
        switch (m_CachePolicy)
        {
            case CachePolicy::Conservative:
                return m_ValidationEnabled;
            
            case CachePolicy::Balanced:
                return m_ValidationEnabled && (m_CurrentFrame % 10) == 0; // Every 10 frames
            
            case CachePolicy::Aggressive:
                return m_ValidationEnabled && (m_CurrentFrame % 60) == 0; // Every 60 frames
            
            case CachePolicy::Minimal:
                return false;
        }
        
        return false;
    }

    void BindingStateCache::PerformAutomaticCleanup()
    {
        OLO_PROFILE_FUNCTION();
        
        auto maxAge = std::chrono::milliseconds(10000); // 10 seconds
        
        switch (m_CachePolicy)
        {
            case CachePolicy::Conservative:
                maxAge = std::chrono::milliseconds(5000);  // 5 seconds
                break;
            case CachePolicy::Balanced:
                maxAge = std::chrono::milliseconds(10000); // 10 seconds
                break;
            case CachePolicy::Aggressive:
                maxAge = std::chrono::milliseconds(30000); // 30 seconds
                break;
            case CachePolicy::Minimal:
                return; // No automatic cleanup
        }
        
        CleanupStaleBindings(maxAge);
    }

    u32 BindingStateCache::GetActualOpenGLBinding(GLenum target, u32 bindingPoint) const
    {
        GLint handle = 0;
        
        switch (target)
        {
            case GL_UNIFORM_BUFFER:
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, bindingPoint, &handle);
                break;
            case GL_SHADER_STORAGE_BUFFER:
                glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, bindingPoint, &handle);
                break;
            case GL_TEXTURE_2D:
                glActiveTexture(GL_TEXTURE0 + bindingPoint);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &handle);
                break;
            // Add more targets as needed
        }
        
        return static_cast<u32>(handle);
    }

    // Global instance access
    BindingStateCache& GetBindingStateCache()
    {
        if (!s_GlobalCache)
        {
            s_GlobalCache = new BindingStateCache();
            s_GlobalCache->Initialize();
        }
        return *s_GlobalCache;
    }
}
