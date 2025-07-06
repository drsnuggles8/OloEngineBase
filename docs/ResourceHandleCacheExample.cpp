/**
 * @file ResourceHandleCacheExample.cpp
 * @brief Comprehensive example demonstrating Phase 6 improvements to the uniform buffer registry system
 * 
 * This example showcases:
 * - Phase 6.1: Resource Handle Caching with pooling and reference counting
 * - Phase 6.2: Enhanced Template Getter with better error handling and type verification
 * - Performance optimizations and best practices
 * 
 * @author OloEngine Team
 * @date July 2025
 */

#include "OloEngine/Renderer/UniformBufferRegistry.h"
#include "OloEngine/Renderer/ResourceHandleCache.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Core/Log.h"

using namespace OloEngine;

/**
 * @brief Example demonstrating basic resource handle caching functionality
 */
void BasicResourceHandleCachingExample()
{
    OLO_CORE_INFO("=== Basic Resource Handle Caching Example ===");
    
    // Create a shader for our registry
    auto shader = CreateRef<Shader>("assets/shaders/PBRMaterial.glsl");
    
    // Configure registry with caching enabled
    UniformBufferRegistrySpecification spec;
    spec.Name = "CachingExample";
    spec.Configuration = RegistryConfiguration::Performance;
    spec.EnableCaching = true;           // Enable handle caching
    spec.EnableHandlePooling = true;     // Enable handle pooling
    spec.MaxCacheSize = 512;            // Cache up to 512 handles
    spec.EnablePerformanceMetrics = true;
    
    // Create registry with caching
    auto registry = CreateScope<UniformBufferRegistry>(shader, spec);
    registry->Initialize();
    
    // Create some uniform buffers
    auto cameraBuffer = CreateRef<UniformBuffer>(sizeof(glm::mat4) * 2, 0); // View + Projection
    auto materialBuffer = CreateRef<UniformBuffer>(256, 1);                 // Material properties
    auto lightingBuffer = CreateRef<UniformBuffer>(1024, 2);               // Lighting data
    
    // Set resources - handles are automatically cached
    registry->SetResource("CameraUniforms", ShaderResourceInput(cameraBuffer));
    registry->SetResource("MaterialUniforms", ShaderResourceInput(materialBuffer));
    registry->SetResource("LightingUniforms", ShaderResourceInput(lightingBuffer));
    
    OLO_CORE_INFO("Resources bound. Handles cached automatically.");
    
    // First access - cache miss, handle is cached
    auto cachedCamera = registry->GetCachedHandle("CameraUniforms");
    if (cachedCamera && cachedCamera->IsValid)
    {
        OLO_CORE_INFO("Camera handle cached: ID={0}, RefCount={1}", 
                     cachedCamera->Handle, cachedCamera->GetRefCount());
    }
    
    // Second access - cache hit
    auto cachedCamera2 = registry->GetCachedHandle("CameraUniforms");
    if (cachedCamera2)
    {
        OLO_CORE_INFO("Cache hit! Same handle retrieved: ID={0}", cachedCamera2->Handle);
    }
    
    // Demonstrate reference counting for shared resources
    registry->AddHandleReference("CameraUniforms");
    registry->AddHandleReference("CameraUniforms");
    
    OLO_CORE_INFO("After adding references: RefCount={0}", cachedCamera->GetRefCount());
    
    // Remove references
    u32 remainingRefs = registry->RemoveHandleReference("CameraUniforms");
    OLO_CORE_INFO("After removing reference: RefCount={0}", remainingRefs);
    
    // Get cache statistics
    auto stats = registry->GetHandleCacheStatistics();
    OLO_CORE_INFO("Cache Stats - Total: {0}, Valid: {1}, Hit Rate: {2:.2f}%", 
                 stats.TotalCachedHandles, stats.ValidHandles, stats.HitRate * 100.0);
}

/**
 * @brief Example demonstrating handle pooling for temporary resources
 */
void HandlePoolingExample()
{
    OLO_CORE_INFO("=== Handle Pooling Example ===");
    
    auto shader = CreateRef<Shader>("assets/shaders/ParticleSystem.glsl");
    
    UniformBufferRegistrySpecification spec;
    spec.Name = "PoolingExample";
    spec.EnableHandlePooling = true;
    spec.MaxPoolSize = 64;              // Pool up to 64 temporary resources
    
    auto registry = CreateScope<UniformBufferRegistry>(shader, spec);
    registry->Initialize();
    
    // Create handle pools for different resource types
    registry->CreateHandlePool<UniformBuffer>(32, []() {
        return CreateRef<UniformBuffer>(256, 0); // 256-byte temp uniform buffers
    });
    
    registry->CreateHandlePool<StorageBuffer>(16, []() {
        return CreateRef<StorageBuffer>(1024); // 1KB temp storage buffers
    });
    
    registry->CreateHandlePool<Texture2D>(8, []() {
        return Texture2D::Create(256, 256); // 256x256 temp textures
    });
    
    OLO_CORE_INFO("Handle pools created for temporary resource management");
    
    // Simulate creating many temporary particle buffers
    std::vector<std::pair<Ref<UniformBuffer>, u32>> tempBuffers;
    
    for (u32 i = 0; i < 10; ++i)
    {
        auto pool = registry->GetHandlePool<UniformBuffer>();
        if (pool)
        {
            auto [buffer, handle] = pool->Acquire();
            if (buffer)
            {
                tempBuffers.push_back({buffer, handle});
                OLO_CORE_INFO("Acquired temp buffer {0}: Handle={1}", i, handle);
            }
        }
    }
    
    // Get pool statistics
    auto pool = registry->GetHandlePool<UniformBuffer>();
    if (pool)
    {
        auto poolStats = pool->GetStats();
        OLO_CORE_INFO("Pool Stats - Total: {0}, InUse: {1}, Available: {2}", 
                     poolStats.TotalResources, poolStats.InUseResources, poolStats.AvailableResources);
    }
    
    // Release temporary buffers back to pool
    for (const auto& [buffer, handle] : tempBuffers)
    {
        if (pool)
        {
            pool->Release(handle);
            OLO_CORE_INFO("Released temp buffer with handle: {0}", handle);
        }
    }
    
    // Clean up old unused resources
    if (pool)
    {
        pool->CleanupOldResources(std::chrono::seconds(30));
        OLO_CORE_INFO("Cleaned up old unused pool resources");
    }
}

/**
 * @brief Example demonstrating enhanced template getter with error handling
 */
void EnhancedTemplateGetterExample()
{
    OLO_CORE_INFO("=== Enhanced Template Getter Example ===");
    
    auto shader = CreateRef<Shader>("assets/shaders/EnhancedMaterial.glsl");
    
    UniformBufferRegistrySpecification spec;
    spec.Name = "EnhancedGetterExample";
    spec.Configuration = RegistryConfiguration::Development; // Better error reporting
    spec.EnableResourceTypeVerification = true;
    spec.EnableAvailabilityChecking = true;
    
    auto registry = CreateScope<UniformBufferRegistry>(shader, spec);
    registry->Initialize();
    
    // Create resources of different types
    auto uniformBuffer = CreateRef<UniformBuffer>(256, 0);
    auto storageBuffer = CreateRef<StorageBuffer>(1024);
    auto texture2D = Texture2D::Create(512, 512);
    auto textureCube = TextureCubemap::Create(256);
    
    registry->SetResource("MaterialUniforms", ShaderResourceInput(uniformBuffer));
    registry->SetResource("InstanceData", ShaderResourceInput(storageBuffer));
    registry->SetResource("DiffuseTexture", ShaderResourceInput(texture2D));
    registry->SetResource("EnvironmentMap", ShaderResourceInput(textureCube));
    
    // Example 1: Successful resource access with enhanced getter
    OLO_CORE_INFO("--- Testing successful resource access ---");
    
    auto result = registry->GetResourceEnhanced<UniformBuffer>("MaterialUniforms");
    if (result.IsSuccess())
    {
        OLO_CORE_INFO("✓ Successfully retrieved UniformBuffer: {0}", result.GetResource()->GetRendererID());
        OLO_CORE_INFO("  Resource type verified at compile-time");
        OLO_CORE_INFO("  Handle cached: {0}", result.WasCached() ? "Yes" : "No");
    }
    
    // Example 2: Type mismatch error handling
    OLO_CORE_INFO("--- Testing type mismatch error handling ---");
    
    auto wrongTypeResult = registry->GetResourceEnhanced<StorageBuffer>("MaterialUniforms");
    if (!wrongTypeResult.IsSuccess())
    {
        OLO_CORE_ERROR("✗ Type mismatch detected: {0}", wrongTypeResult.GetErrorMessage());
        OLO_CORE_ERROR("  Expected: StorageBuffer, Found: UniformBuffer");
        OLO_CORE_ERROR("  Error Code: {0}", static_cast<i32>(wrongTypeResult.GetErrorCode()));
    }
    
    // Example 3: Resource not found handling
    OLO_CORE_INFO("--- Testing missing resource error handling ---");
    
    auto missingResult = registry->GetResourceEnhanced<Texture2D>("NonexistentTexture");
    if (!missingResult.IsSuccess())
    {
        OLO_CORE_ERROR("✗ Resource not found: {0}", missingResult.GetErrorMessage());
        OLO_CORE_ERROR("  Suggestion: {0}", missingResult.GetSuggestion());
    }
    
    // Example 4: Smart resource conversion
    OLO_CORE_INFO("--- Testing smart resource conversion ---");
    
    // Create a UniformBufferSet from individual UniformBuffer
    auto conversionResult = registry->GetResourceEnhanced<UniformBuffer>("MaterialUniforms");
    if (conversionResult.IsSuccess())
    {
        // The enhanced getter can suggest converting to UniformBufferSet if needed
        if (conversionResult.SuggestsConversion())
        {
            OLO_CORE_INFO("💡 Conversion suggestion: {0}", conversionResult.GetConversionSuggestion());
        }
    }
    
    // Example 5: Resource availability checking
    OLO_CORE_INFO("--- Testing resource availability checking ---");
    
    bool isReady = registry->IsResourceReady<Texture2D>("DiffuseTexture");
    OLO_CORE_INFO("DiffuseTexture ready: {0}", isReady ? "Yes" : "No");
    
    // Example 6: Fallback resource access
    OLO_CORE_INFO("--- Testing fallback resource access ---");
    
    auto fallbackTexture = Texture2D::Create(1, 1); // White 1x1 fallback
    auto textureWithFallback = registry->GetResourceOrFallback<Texture2D>("MissingTexture", fallbackTexture);
    if (textureWithFallback)
    {
        OLO_CORE_INFO("✓ Fallback texture used for missing resource");
    }
    
    // Example 7: Factory-based resource creation
    OLO_CORE_INFO("--- Testing factory-based resource creation ---");
    
    auto createdBuffer = registry->GetOrCreateResource<UniformBuffer>("DynamicBuffer", []() {
        OLO_CORE_INFO("Creating new UniformBuffer via factory");
        return CreateRef<UniformBuffer>(512, 3);
    });
    
    if (createdBuffer)
    {
        OLO_CORE_INFO("✓ Resource created via factory: ID={0}", createdBuffer->GetRendererID());
    }
}

/**
 * @brief Example demonstrating performance optimizations and cache invalidation
 */
void PerformanceOptimizationExample()
{
    OLO_CORE_INFO("=== Performance Optimization Example ===");
    
    auto shader = CreateRef<Shader>("assets/shaders/HighPerformance.glsl");
    
    UniformBufferRegistrySpecification spec;
    spec.Name = "PerformanceExample";
    spec.Configuration = RegistryConfiguration::Performance;
    spec.EnableCaching = true;
    spec.EnableBatching = true;
    spec.MaxCacheSize = 1024;
    spec.CacheCleanupInterval = std::chrono::minutes(2);
    
    auto registry = CreateScope<UniformBufferRegistry>(shader, spec);
    registry->Initialize();
    
    // Create many resources to demonstrate caching benefits
    std::vector<Ref<UniformBuffer>> buffers;
    for (u32 i = 0; i < 100; ++i)
    {
        auto buffer = CreateRef<UniformBuffer>(256, i);
        buffers.push_back(buffer);
        registry->SetResource("Buffer" + std::to_string(i), ShaderResourceInput(buffer));
    }
    
    OLO_CORE_INFO("Created 100 uniform buffers");
    
    // Measure cache performance
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Access resources many times (should hit cache)
    for (u32 iteration = 0; iteration < 1000; ++iteration)
    {
        for (u32 i = 0; i < 100; ++i)
        {
            auto handle = registry->GetCachedHandle("Buffer" + std::to_string(i));
            // Handle access is very fast due to caching
        }
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    OLO_CORE_INFO("100,000 cache accesses completed in {0} microseconds", duration.count());
    
    // Get performance statistics
    auto stats = registry->GetHandleCacheStatistics();
    OLO_CORE_INFO("Final Cache Stats:");
    OLO_CORE_INFO("  Total Handles: {0}", stats.TotalCachedHandles);
    OLO_CORE_INFO("  Valid Handles: {0}", stats.ValidHandles);
    OLO_CORE_INFO("  Hit Rate: {0:.2f}%", stats.HitRate * 100.0);
    OLO_CORE_INFO("  Total Memory: {0} bytes", stats.TotalMemorySize);
    
    // Demonstrate cache invalidation
    OLO_CORE_INFO("--- Testing cache invalidation ---");
    
    // Invalidate specific handles when resources change
    registry->InvalidateCachedHandle("Buffer50");
    OLO_CORE_INFO("Invalidated handle for Buffer50");
    
    // Invalidate by type (useful for bulk operations)
    registry->InvalidateHandlesByType(ShaderResourceType::UniformBuffer);
    OLO_CORE_INFO("Invalidated all UniformBuffer handles");
    
    // Clean up cache periodically
    registry->CleanupHandleCache(512, std::chrono::minutes(5));
    OLO_CORE_INFO("Cache cleanup completed");
    
    auto finalStats = registry->GetHandleCacheStatistics();
    OLO_CORE_INFO("Post-cleanup: Valid={0}, Invalid={1}", 
                 finalStats.ValidHandles, finalStats.InvalidHandles);
}

/**
 * @brief Example demonstrating integration with frame-in-flight systems
 */
void FrameInFlightIntegrationExample()
{
    OLO_CORE_INFO("=== Frame-in-Flight Integration Example ===");
    
    auto shader = CreateRef<Shader>("assets/shaders/FrameInFlight.glsl");
    
    UniformBufferRegistrySpecification spec;
    spec.Name = "FrameInFlightExample";
    spec.EnableFrameInFlight = true;
    spec.FramesInFlight = 3;
    spec.EnableCaching = true;
    spec.EnableHandlePooling = true;
    
    auto registry = CreateScope<UniformBufferRegistry>(shader, spec);
    registry->Initialize();
    
    // Register frame-in-flight resources
    registry->RegisterFrameInFlightResource("CameraData", ShaderResourceType::UniformBuffer, 
                                           sizeof(glm::mat4) * 2, BufferUsage::Dynamic);
    registry->RegisterFrameInFlightResource("MaterialData", ShaderResourceType::UniformBuffer, 
                                           256, BufferUsage::Dynamic);
    
    OLO_CORE_INFO("Registered frame-in-flight resources");
    
    // Simulate multiple frames
    for (u32 frame = 0; frame < 6; ++frame)
    {
        OLO_CORE_INFO("--- Frame {0} ---", frame);
        
        // Get current frame resources (automatically handles multiple buffers)
        auto cameraBuffer = registry->GetCurrentFrameResource<UniformBuffer>("CameraData");
        auto materialBuffer = registry->GetCurrentFrameResource<UniformBuffer>("MaterialData");
        
        if (cameraBuffer && materialBuffer)
        {
            OLO_CORE_INFO("Frame {0}: Camera ID={1}, Material ID={2}", 
                         frame, cameraBuffer->GetRendererID(), materialBuffer->GetRendererID());
            
            // Update buffers with frame-specific data
            // (In real code, you'd update with actual camera/material data)
            glm::mat4 viewMatrix = glm::mat4(1.0f);
            glm::mat4 projMatrix = glm::mat4(1.0f);
            
            cameraBuffer->SetData(&viewMatrix, sizeof(glm::mat4), 0);
            cameraBuffer->SetData(&projMatrix, sizeof(glm::mat4), sizeof(glm::mat4));
            
            // Cache handles for performance
            auto cachedHandle = registry->GetCachedHandle("CameraData");
            if (cachedHandle)
            {
                cachedHandle->Touch(); // Update access time
                OLO_CORE_INFO("  Cached handle touched: RefCount={0}", cachedHandle->GetRefCount());
            }
        }
        
        // Advance to next frame
        registry->NextFrame();
    }
    
    // Get frame-in-flight statistics
    auto frameStats = registry->GetFrameInFlightStatistics();
    OLO_CORE_INFO("Frame-in-Flight Stats:");
    OLO_CORE_INFO("  Registered Resources: {0}", frameStats.RegisteredResources);
    OLO_CORE_INFO("  Active Buffers: {0}", frameStats.ActiveBuffers);
    OLO_CORE_INFO("  Total Memory: {0} bytes", frameStats.TotalMemoryUsage);
}

/**
 * @brief Example demonstrating best practices and advanced usage patterns
 */
void BestPracticesExample()
{
    OLO_CORE_INFO("=== Best Practices Example ===");
    
    auto shader = CreateRef<Shader>("assets/shaders/BestPractices.glsl");
    
    // Configure for optimal performance
    UniformBufferRegistrySpecification spec;
    spec.Name = "BestPracticesRegistry";
    spec.Configuration = RegistryConfiguration::Performance;
    
    // Enable all performance features
    spec.EnableCaching = true;
    spec.EnableHandlePooling = true;
    spec.EnableBatching = true;
    spec.EnableFrameInFlight = true;
    spec.FramesInFlight = 3;
    
    // Optimize cache settings
    spec.MaxCacheSize = 2048;
    spec.MaxPoolSize = 128;
    spec.CacheCleanupInterval = std::chrono::minutes(5);
    
    // Enable performance monitoring
    spec.EnablePerformanceMetrics = true;
    spec.EnableResourceProfiling = true;
    
    auto registry = CreateScope<UniformBufferRegistry>(shader, spec);
    registry->Initialize();
    
    OLO_CORE_INFO("✓ Registry configured for optimal performance");
    
    // Best Practice 1: Pre-create handle pools for known resource types
    registry->CreateHandlePool<UniformBuffer>(64, []() {
        return CreateRef<UniformBuffer>(256, 0); // Common size for material uniforms
    });
    
    registry->CreateHandlePool<Texture2D>(32, []() {
        return Texture2D::Create(1024, 1024); // Common texture size
    });
    
    OLO_CORE_INFO("✓ Handle pools pre-created for common resource types");
    
    // Best Practice 2: Use enhanced getters with proper error handling
    auto materialResult = registry->GetResourceEnhanced<UniformBuffer>("MaterialUniforms");
    if (!materialResult.IsSuccess())
    {
        // Create resource if it doesn't exist
        auto materialBuffer = CreateRef<UniformBuffer>(256, 1);
        registry->SetResource("MaterialUniforms", ShaderResourceInput(materialBuffer));
        OLO_CORE_INFO("✓ Created missing MaterialUniforms buffer");
    }
    
    // Best Practice 3: Batch resource operations for better performance
    registry->BeginBatch();
    {
        // Set multiple resources in a batch
        auto diffuseTexture = Texture2D::Create(512, 512);
        auto normalTexture = Texture2D::Create(512, 512);
        auto roughnessTexture = Texture2D::Create(512, 512);
        
        registry->SetResource("DiffuseTexture", ShaderResourceInput(diffuseTexture));
        registry->SetResource("NormalTexture", ShaderResourceInput(normalTexture));
        registry->SetResource("RoughnessTexture", ShaderResourceInput(roughnessTexture));
    }
    registry->EndBatch();
    
    OLO_CORE_INFO("✓ Batch resource operations completed");
    
    // Best Practice 4: Monitor performance and cache efficiency
    auto stats = registry->GetHandleCacheStatistics();
    if (stats.HitRate < 0.8) // If hit rate is below 80%
    {
        OLO_CORE_WARN("Cache hit rate is low: {0:.2f}%. Consider increasing cache size.", 
                     stats.HitRate * 100.0);
    }
    
    // Best Practice 5: Regular cleanup and maintenance
    registry->CleanupHandleCache(1024, std::chrono::minutes(10));
    
    // Clean up old pooled resources
    auto uniformPool = registry->GetHandlePool<UniformBuffer>();
    if (uniformPool)
    {
        uniformPool->CleanupOldResources(std::chrono::minutes(5));
    }
    
    OLO_CORE_INFO("✓ Regular maintenance completed");
    
    // Best Practice 6: Profile resource usage
    auto updateStats = registry->GetUpdateStatistics();
    OLO_CORE_INFO("Update Statistics:");
    OLO_CORE_INFO("  Batch Operations: {0}", updateStats.BatchedOperations);
    OLO_CORE_INFO("  Deferred Updates: {0}", updateStats.DeferredUpdates);
    OLO_CORE_INFO("  Average Batch Size: {0:.2f}", updateStats.AverageBatchSize);
    
    OLO_CORE_INFO("✓ Performance profiling completed");
}

/**
 * @brief Main function demonstrating all Phase 6 improvements
 */
i32 main()
{
    // Initialize OloEngine logging
    Log::Init();
    
    OLO_CORE_INFO("Starting ResourceHandleCache Phase 6 Examples");
    OLO_CORE_INFO("=====================================================");
    
    try
    {
        // Run all examples
        BasicResourceHandleCachingExample();
        HandlePoolingExample();
        EnhancedTemplateGetterExample();
        PerformanceOptimizationExample();
        FrameInFlightIntegrationExample();
        BestPracticesExample();
        
        OLO_CORE_INFO("=====================================================");
        OLO_CORE_INFO("All Phase 6 examples completed successfully!");
    }
    catch (const std::exception& e)
    {
        OLO_CORE_ERROR("Example failed with exception: {0}", e.what());
        return -1;
    }
    
    return 0;
}

/**
 * @brief Additional utility functions for advanced usage patterns
 */
namespace AdvancedPatterns
{
    /**
     * @brief RAII wrapper for automatic handle pool resource management
     */
    template<typename T>
    class PooledResource
    {
    private:
        HandlePool<T>* m_Pool;
        Ref<T> m_Resource;
        u32 m_Handle;
        bool m_IsValid;

    public:
        PooledResource(HandlePool<T>* pool) : m_Pool(pool), m_Handle(0), m_IsValid(false)
        {
            if (m_Pool)
            {
                auto [resource, handle] = m_Pool->Acquire();
                if (resource)
                {
                    m_Resource = resource;
                    m_Handle = handle;
                    m_IsValid = true;
                }
            }
        }
        
        ~PooledResource()
        {
            if (m_IsValid && m_Pool)
            {
                m_Pool->Release(m_Handle);
            }
        }
        
        // No copy semantics
        PooledResource(const PooledResource&) = delete;
        PooledResource& operator=(const PooledResource&) = delete;
        
        // Move semantics
        PooledResource(PooledResource&& other) noexcept
            : m_Pool(other.m_Pool), m_Resource(std::move(other.m_Resource)), 
              m_Handle(other.m_Handle), m_IsValid(other.m_IsValid)
        {
            other.m_IsValid = false;
        }
        
        PooledResource& operator=(PooledResource&& other) noexcept
        {
            if (this != &other)
            {
                if (m_IsValid && m_Pool)
                {
                    m_Pool->Release(m_Handle);
                }
                
                m_Pool = other.m_Pool;
                m_Resource = std::move(other.m_Resource);
                m_Handle = other.m_Handle;
                m_IsValid = other.m_IsValid;
                other.m_IsValid = false;
            }
            return *this;
        }
        
        Ref<T> GetResource() const { return m_Resource; }
        u32 GetHandle() const { return m_Handle; }
        bool IsValid() const { return m_IsValid; }
        
        Ref<T> operator->() const { return m_Resource; }
        T& operator*() const { return *m_Resource; }
    };
    
    /**
     * @brief Example of using the RAII wrapper
     */
    void RAIIPoolExample(UniformBufferRegistry* registry)
    {
        auto pool = registry->GetHandlePool<UniformBuffer>();
        if (pool)
        {
            // Automatic resource management with RAII
            PooledResource<UniformBuffer> tempBuffer(pool);
            if (tempBuffer.IsValid())
            {
                // Use the buffer
                tempBuffer->SetData(nullptr, 256, 0);
                OLO_CORE_INFO("Using temporary buffer: ID={0}", tempBuffer.GetHandle());
                
                // Buffer is automatically returned to pool when tempBuffer goes out of scope
            }
        }
    }
    
    /**
     * @brief Smart cache warming strategy
     */
    void WarmCacheExample(UniformBufferRegistry* registry)
    {
        // Pre-warm cache with commonly accessed resources
        std::vector<std::string> commonResources = {
            "MaterialUniforms", "CameraUniforms", "LightingData",
            "ShadowMaps", "EnvironmentMap", "BRDFLookup"
        };
        
        for (const auto& resourceName : commonResources)
        {
            // Access each resource to ensure it's cached
            auto handle = registry->GetCachedHandle(resourceName);
            if (handle)
            {
                handle->Touch(); // Update access time
            }
        }
        
        OLO_CORE_INFO("Cache warmed with {0} common resources", commonResources.size());
    }
}
