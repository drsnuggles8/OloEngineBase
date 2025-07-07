#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RendererTypes.h"
#include <unordered_map>
#include <vector>
#include <chrono>
#include <glad/gl.h>

namespace OloEngine
{
    // Forward declarations
    class UniformBufferRegistry;
    struct ShaderResourceBinding;

    /**
     * @brief Cached binding state for a single resource
     */
    struct CachedBindingState
    {
        u32 ResourceHandle = 0;         // OpenGL handle (buffer, texture, etc.)
        u32 BindingPoint = 0;           // Binding point index
        u32 Set = 0;                    // Descriptor set index
        GLenum Target = 0;              // OpenGL target (GL_UNIFORM_BUFFER, etc.)
        ShaderResourceType Type = ShaderResourceType::None;
        
        // Binding parameters
        GLintptr Offset = 0;            // Buffer offset for range bindings
        GLsizeiptr Size = 0;            // Buffer size for range bindings
        size_t ResourceSize = 0;        // Total resource size
        
        // State tracking
        bool IsActive = false;          // Currently bound in OpenGL
        bool IsDirty = true;            // Needs rebinding
        bool IsValidated = false;       // Has been validated against shader
        
        // Performance tracking
        u32 LastBoundFrame = 0;         // Frame when last bound
        u32 BindCount = 0;              // Total number of times bound
        u32 AccessCount = 0;            // Times accessed/checked
        std::chrono::steady_clock::time_point LastAccessed;
        std::chrono::steady_clock::time_point FirstBound;
        
        // Hash for quick comparison
        u64 StateHash = 0;              // Hash of current state
        
        CachedBindingState() = default;
        CachedBindingState(u32 handle, u32 binding, GLenum target, ShaderResourceType type)
            : ResourceHandle(handle), BindingPoint(binding), Target(target), Type(type),
              LastAccessed(std::chrono::steady_clock::now()) {}
        
        /**
         * @brief Calculate hash of current binding state for fast comparison
         */
        u64 CalculateHash() const;
        
        /**
         * @brief Check if this state matches another state (for redundancy checking)
         */
        bool MatchesState(const CachedBindingState& other) const;
        
        /**
         * @brief Update access tracking
         */
        void UpdateAccess();
        
        /**
         * @brief Mark as bound and update tracking
         */
        void MarkBound(u32 frameNumber);
        
        /**
         * @brief Check if binding is stale (not accessed recently)
         */
        bool IsStale(std::chrono::milliseconds maxAge = std::chrono::milliseconds(5000)) const;
    };

    /**
     * @brief Global binding state for tracking OpenGL state
     */
    struct GlobalBindingState
    {
        // Current OpenGL state
        u32 ActiveTextureUnit = 0;      // Currently active texture unit
        u32 CurrentProgram = 0;         // Currently bound shader program
        u32 CurrentVAO = 0;             // Currently bound vertex array object
        
        // Binding point states
        std::unordered_map<u32, u32> UniformBufferBindings;    // binding point -> buffer handle
        std::unordered_map<u32, u32> StorageBufferBindings;    // binding point -> buffer handle
        std::unordered_map<u32, u32> TextureBindings;          // texture unit -> texture handle
        std::unordered_map<u32, u32> ImageBindings;            // image unit -> texture handle
        
        // State validity
        bool IsValid = true;            // Whether cached state is accurate
        u32 LastValidationFrame = 0;    // Frame when last validated
        
        /**
         * @brief Invalidate all cached state
         */
        void Invalidate();
        
        /**
         * @brief Validate cached state against actual OpenGL state
         */
        bool ValidateAgainstOpenGL();
        
        /**
         * @brief Update binding in cached state
         */
        void UpdateBinding(GLenum target, u32 bindingPoint, u32 handle);
    };

    /**
     * @brief Statistics for binding state cache performance
     */
    struct BindingCacheStatistics
    {
        u32 TotalCacheHits = 0;         // Redundant bindings avoided
        u32 TotalCacheMisses = 0;       // Bindings that were necessary
        u32 StateValidations = 0;       // Times state was validated
        u32 CacheInvalidations = 0;    // Times cache was invalidated
        u32 StaleBindingsRemoved = 0;   // Stale bindings cleaned up
        f32 HitRate = 0.0f;             // Cache hit percentage
        f32 AverageBindTime = 0.0f;     // Average time per bind operation
        f32 TimeSaved = 0.0f;           // Total time saved by cache (ms)
        
        void Reset()
        {
            TotalCacheHits = TotalCacheMisses = StateValidations = 0;
            CacheInvalidations = StaleBindingsRemoved = 0;
            HitRate = AverageBindTime = TimeSaved = 0.0f;
        }
        
        void UpdateHitRate()
        {
            u32 total = TotalCacheHits + TotalCacheMisses;
            HitRate = (total > 0) ? (static_cast<f32>(TotalCacheHits) / static_cast<f32>(total)) * 100.0f : 0.0f;
        }
    };

    /**
     * @brief Enhanced binding state cache for avoiding redundant OpenGL calls
     * 
     * This system tracks all binding state across the application to eliminate
     * redundant glBindBuffer, glBindTexture, and other binding calls.
     */
    class BindingStateCache
    {
    public:
        /**
         * @brief Cache invalidation strategies
         */
        enum class InvalidationStrategy
        {
            Immediate,      // Invalidate immediately when resource changes
            FrameBased,     // Invalidate at frame boundaries
            TimeBased,      // Invalidate after time threshold
            Manual          // Only invalidate when explicitly requested
        };

        /**
         * @brief Cache management policies
         */
        enum class CachePolicy
        {
            Conservative,   // Cache everything, validate frequently
            Balanced,       // Balance between performance and accuracy
            Aggressive,     // Cache aggressively, validate rarely
            Minimal         // Minimal caching for debugging
        };

        BindingStateCache();
        ~BindingStateCache();

        // Disable copy semantics
        BindingStateCache(const BindingStateCache&) = delete;
        BindingStateCache& operator=(const BindingStateCache&) = delete;

        /**
         * @brief Initialize the binding state cache
         * @param policy Cache management policy
         * @param strategy Invalidation strategy
         * @return True if initialization was successful
         */
        bool Initialize(CachePolicy policy = CachePolicy::Balanced, 
                       InvalidationStrategy strategy = InvalidationStrategy::FrameBased);

        /**
         * @brief Shutdown the cache and free resources
         */
        void Shutdown();

        /**
         * @brief Check if a binding would be redundant
         * @param target OpenGL target (GL_UNIFORM_BUFFER, etc.)
         * @param bindingPoint Binding point index
         * @param resourceHandle Resource handle to bind
         * @param offset Buffer offset (for range bindings)
         * @param size Buffer size (for range bindings)
         * @return True if binding would be redundant
         */
        bool IsBindingRedundant(GLenum target, u32 bindingPoint, u32 resourceHandle, 
                              GLintptr offset = 0, GLsizeiptr size = 0);

        /**
         * @brief Record a successful binding operation
         * @param target OpenGL target
         * @param bindingPoint Binding point index
         * @param resourceHandle Resource handle that was bound
         * @param resourceType Shader resource type
         * @param offset Buffer offset
         * @param size Buffer size
         * @param frameNumber Current frame number
         */
        void RecordBinding(GLenum target, u32 bindingPoint, u32 resourceHandle, 
                          ShaderResourceType resourceType, GLintptr offset = 0, 
                          GLsizeiptr size = 0, u32 frameNumber = 0);

        /**
         * @brief Apply cached bindings from a registry
         * @param registry Registry containing resources to bind
         * @param forceRebind Whether to force rebinding even if cached
         * @return Number of bindings that were actually applied (not cached)
         */
        u32 ApplyRegistryBindings(const UniformBufferRegistry& registry, bool forceRebind = false);

        /**
         * @brief Invalidate specific binding
         * @param target OpenGL target
         * @param bindingPoint Binding point to invalidate
         */
        void InvalidateBinding(GLenum target, u32 bindingPoint);

        /**
         * @brief Invalidate all bindings of a specific type
         * @param target OpenGL target to invalidate
         */
        void InvalidateBindingsOfType(GLenum target);

        /**
         * @brief Invalidate all cached binding state
         */
        void InvalidateAllBindings();

        /**
         * @brief Update frame counter for frame-based invalidation
         * @param frameNumber Current frame number
         */
        void SetCurrentFrame(u32 frameNumber);

        /**
         * @brief Clean up stale bindings that haven't been accessed recently
         * @param maxAge Maximum age before considering binding stale
         * @return Number of stale bindings removed
         */
        u32 CleanupStaleBindings(std::chrono::milliseconds maxAge = std::chrono::milliseconds(10000));

        /**
         * @brief Validate cache against actual OpenGL state
         * @param fullValidation Whether to validate all bindings or just sample
         * @return True if cache is accurate
         */
        bool ValidateCache(bool fullValidation = false);

        /**
         * @brief Force synchronization with actual OpenGL state
         */
        void SynchronizeWithOpenGL();

        /**
         * @brief Get cached binding for a specific point
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @return Pointer to cached state, or nullptr if not cached
         */
        const CachedBindingState* GetCachedBinding(GLenum target, u32 bindingPoint) const;

        /**
         * @brief Get all cached bindings of a specific type
         * @param target OpenGL target
         * @return Vector of cached binding states
         */
        std::vector<const CachedBindingState*> GetCachedBindingsOfType(GLenum target) const;

        /**
         * @brief Set cache policy
         * @param policy New cache policy
         */
        void SetCachePolicy(CachePolicy policy) { m_CachePolicy = policy; }

        /**
         * @brief Set invalidation strategy
         * @param strategy New invalidation strategy
         */
        void SetInvalidationStrategy(InvalidationStrategy strategy) { m_InvalidationStrategy = strategy; }

        /**
         * @brief Enable or disable cache validation
         * @param enabled Whether to perform validation checks
         */
        void SetValidationEnabled(bool enabled) { m_ValidationEnabled = enabled; }

        /**
         * @brief Enable or disable automatic cleanup of stale bindings
         * @param enabled Whether to automatically clean up stale bindings
         * @param interval Cleanup interval in frames
         */
        void SetAutomaticCleanup(bool enabled, u32 interval = 60) { 
            m_AutoCleanupEnabled = enabled; 
            m_CleanupInterval = interval;
        }

        /**
         * @brief Get cache performance statistics
         * @return Current statistics
         */
        const BindingCacheStatistics& GetStatistics() const { return m_Statistics; }

        /**
         * @brief Reset cache statistics
         */
        void ResetStatistics() { m_Statistics.Reset(); }

        /**
         * @brief Get global OpenGL binding state
         * @return Reference to global state tracker
         */
        const GlobalBindingState& GetGlobalState() const { return m_GlobalState; }

        /**
         * @brief Get cache size information
         */
        struct CacheInfo
        {
            u32 TotalBindings = 0;
            u32 ActiveBindings = 0;
            u32 StaleBindings = 0;
            size_t MemoryUsage = 0;     // Estimated memory usage in bytes
        };

        CacheInfo GetCacheInfo() const;

    private:
        // Configuration
        CachePolicy m_CachePolicy = CachePolicy::Balanced;
        InvalidationStrategy m_InvalidationStrategy = InvalidationStrategy::FrameBased;
        bool m_IsInitialized = false;
        bool m_ValidationEnabled = true;
        bool m_AutoCleanupEnabled = true;
        u32 m_CleanupInterval = 60;     // Frames between automatic cleanup
        u32 m_CurrentFrame = 0;
        u32 m_LastCleanupFrame = 0;

        // Cache storage
        std::unordered_map<u64, CachedBindingState> m_BindingCache; // Key: (target << 32) | bindingPoint
        GlobalBindingState m_GlobalState;

        // Performance tracking
        mutable BindingCacheStatistics m_Statistics;

        /**
         * @brief Generate unique key for cache storage
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @return 64-bit unique key
         */
        u64 GenerateCacheKey(GLenum target, u32 bindingPoint) const;

        /**
         * @brief Update statistics for cache operation
         * @param wasHit Whether the cache was hit
         * @param bindTime Time taken for binding operation
         */
        void UpdateStatistics(bool wasHit, f32 bindTime = 0.0f) const;

        /**
         * @brief Check if binding state should be cached based on policy
         * @param target OpenGL target
         * @param resourceType Shader resource type
         * @return True if should be cached
         */
        bool ShouldCache(GLenum target, ShaderResourceType resourceType) const;

        /**
         * @brief Check if cache validation is needed
         * @return True if validation should be performed
         */
        bool ShouldValidate() const;

        /**
         * @brief Perform automatic cleanup if needed
         */
        void PerformAutomaticCleanup();

        /**
         * @brief Get the actual OpenGL binding for validation
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @return Currently bound handle from OpenGL
         */
        u32 GetActualOpenGLBinding(GLenum target, u32 bindingPoint) const;
    };

    /**
     * @brief Global binding state cache instance
     * @return Reference to global cache
     */
    BindingStateCache& GetBindingStateCache();
}
