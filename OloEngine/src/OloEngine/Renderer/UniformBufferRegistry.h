#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ShaderResourceTypes.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/ArrayResource.h"
#include "OloEngine/Renderer/FrameInFlightManager.h"
#include "OloEngine/Renderer/BindingStateCache.h"
#include "Platform/OpenGL/OpenGLMultiBind.h"
#include "Platform/OpenGL/OpenGLResourceDeclaration.h"
#include "Platform/OpenGL/OpenGLDSABindingManager.h"
#include "Platform/OpenGL/OpenGLDescriptorSetManager.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <memory>
#include <chrono>

#include <glad/gl.h>

namespace OloEngine
{
    // Forward declarations to avoid circular dependencies
    class ResourceHandleCache;
    template<typename T> struct ResourceAccessResult;
    class EnhancedResourceGetter;
    class OpenGLDescriptorSetManager;
    class Shader;

    /**
     * @brief Phase 3.1: Descriptor set priority levels for resource organization
     */
    enum class DescriptorSetPriority : u8
    {
        System = 0,      // Engine-level resources (view/projection matrices, time, etc.)
        Global = 1,      // Scene-level resources (lighting, environment maps, etc.)
        Material = 2,    // Material-specific resources (diffuse/normal textures, material properties)
        Instance = 3,    // Per-instance resources (model matrices, instance data)
        Custom = 4       // User-defined sets for special cases
    };

    /**
     * @brief Phase 4.1: Invalidation reasons for debugging and dependency tracking
     */
    enum class InvalidationReason : u8
    {
        Unknown = 0,
        UserRequested,       // Explicitly invalidated by user
        DependencyChanged,   // Invalidated because a dependency changed
        ResourceUpdated,     // Resource content was updated
        ShaderChanged,       // Shader was recompiled/changed
        FrameInFlightSwap,   // Frame-in-flight buffer swap
        SizeChanged,         // Resource size changed
        FormatChanged,       // Resource format changed
        OptimizationPass     // Invalidated during optimization
    };

    /**
     * @brief Phase 4.2: Update priority levels for batch processing
     */
    enum class UpdatePriority : u8
    {
        Immediate = 0,   // Must be updated immediately
        High = 1,        // Update in next batch
        Normal = 2,      // Standard priority
        Low = 3,         // Can be deferred
        Background = 4   // Update when convenient
    };

    /**
     * @brief Phase 5.1: Resource usage tracking for analytics and optimization
     */
    enum class ResourceUsagePattern : u8
    {
        Unknown = 0,
        StaticWrite,     // Written once, read many times (e.g., uniform buffers)
        DynamicWrite,    // Frequently updated (e.g., per-frame uniforms)
        Streaming,       // Continuous updates (e.g., animation data)
        ReadOnly,        // Only read from (e.g., static textures)
        ReadWrite,       // Both read and written (e.g., SSBOs)
        Compute,         // Used in compute shaders
        Temporal         // Changes over time predictably
    };

    /**
     * @brief Phase 5.2: Validation severity levels for graduated error handling
     */
    enum class RegistryValidationSeverity : u8
    {
        Info = 0,        // Informational messages
        Warning = 1,     // Potential issues that don't break functionality
        Error = 2,       // Issues that may cause rendering problems
        Critical = 3     // Severe issues that will cause crashes or corruption
    };

    /**
     * @brief Phase 3.1: Descriptor set configuration for multi-set resource management
     */
    struct DescriptorSetInfo
    {
        u32 SetIndex;
        DescriptorSetPriority Priority;
        std::string Name;
        std::vector<std::string> ResourceNames;
        bool IsActive = true;
        u32 BindFrequency = 0;  // How often this set gets bound (for optimization)
        
        DescriptorSetInfo() = default;
        DescriptorSetInfo(u32 index, DescriptorSetPriority priority, const std::string& name)
            : SetIndex(index), Priority(priority), Name(name) {}
    };

    /**
     * @brief Phase 2.2: Configuration presets for different use cases
     */
    enum class RegistryConfiguration : u8
    {
        Debug = 0,        // Maximum validation, detailed logging
        Release,          // Optimized with basic validation
        Performance,      // Minimal overhead, validation disabled
        Development       // Balanced validation and performance
    };

    /**
     * @brief Phase 2.2: Specification for UniformBufferRegistry creation and configuration
     */
    struct UniformBufferRegistrySpecification
    {
        // Core configuration
        std::string Name = "UniformBufferRegistry";
        RegistryConfiguration Configuration = RegistryConfiguration::Development;
        
        // Phase 3.1: Multi-set management (NOW ENABLED BY DEFAULT)
        u32 StartSet = 0;                      // Starting descriptor set number
        u32 EndSet = 4;                        // Ending descriptor set number (covers System->Custom)
        bool UseSetPriority = true;            // Enable set-based priority system (DEFAULT: true)
        bool EnableSetOptimization = true;     // Optimize binding order based on set priority (DEFAULT: true)
        bool AutoAssignSets = true;            // Automatically assign resources to appropriate sets (DEFAULT: true)
        
        // Phase 3.2: Default resources (NOW ENABLED BY DEFAULT)
        bool EnableDefaultResources = true;    // Auto-populate common resources (DEFAULT: true)
        bool UseResourceTemplates = true;      // Use template library for common patterns (DEFAULT: true)
        bool AutoDetectShaderPattern = true;   // Smart defaults based on shader reflection (DEFAULT: true)
        bool CreateSystemDefaults = true;      // Create default system resources (DEFAULT: true)
        
        // Validation settings
        bool EnableValidation = true;          // Enable resource validation
        bool EnableConflictDetection = true;   // Check for binding conflicts
        bool EnableSizeValidation = true;      // Validate resource sizes
        bool EnableLifecycleTracking = true;   // Track resource lifecycle
        bool EnableSetValidation = true;       // Validate descriptor set assignments (NEW)
        
        // Performance settings
        bool EnableCaching = true;             // Cache frequently accessed handles
        bool EnableBatching = true;            // Batch resource updates
        bool EnableFrameInFlight = false;     // Enable frame-in-flight management
        u32 FramesInFlight = 3;               // Number of frames in flight
        bool EnableSetBatching = true;        // Batch operations by descriptor set (NEW)
        
        // Debug settings
        bool EnableDebugInterface = true;      // ImGui debug interface
        bool EnablePerformanceMetrics = true; // Track performance statistics
        bool EnableResourceProfiling = true;  // Profile resource usage
        bool EnableSetVisualization = true;   // Visualize descriptor set organization (NEW)
        
        // Advanced features
        bool EnableInvalidationTracking = true; // Granular invalidation tracking
        bool EnableDependencyTracking = true;   // Track resource dependencies
        bool EnableAutoOptimization = true;     // Automatic optimization based on usage
        bool EnableSetPriorityOptimization = true; // Optimize based on set priorities (NEW)
        
        // Template and cloning support (Phase 2.1)
        bool AllowTemplateCreation = true;     // Allow creating templates from this registry
        bool AllowCloning = true;              // Allow cloning this registry
        std::string TemplateSource;            // Source template name (for cloned registries)
        
        /**
         * @brief Validate the specification settings
         */
        bool Validate() const
        {
            if (StartSet > EndSet)
            {
                OLO_CORE_ERROR("RegistrySpec: StartSet ({0}) cannot be greater than EndSet ({1})", StartSet, EndSet);
                return false;
            }
            
            if (FramesInFlight == 0 && EnableFrameInFlight)
            {
                OLO_CORE_ERROR("RegistrySpec: FramesInFlight cannot be 0 when EnableFrameInFlight is true");
                return false;
            }
            
            if (FramesInFlight > 10)
            {
                OLO_CORE_WARN("RegistrySpec: FramesInFlight ({0}) is unusually high, consider reducing for memory efficiency", FramesInFlight);
            }
            
            return true;
        }
        
        /**
         * @brief Get configuration preset
         */
        static UniformBufferRegistrySpecification GetPreset(RegistryConfiguration config)
        {
            UniformBufferRegistrySpecification spec;
            spec.Configuration = config;
            
            switch (config)
            {
                case RegistryConfiguration::Debug:
                    spec.EnableValidation = true;
                    spec.EnableConflictDetection = true;
                    spec.EnableSizeValidation = true;
                    spec.EnableLifecycleTracking = true;
                    spec.EnableDebugInterface = true;
                    spec.EnablePerformanceMetrics = true;
                    spec.EnableResourceProfiling = true;
                    spec.EnableInvalidationTracking = true;
                    spec.EnableDependencyTracking = true;
                    break;
                    
                case RegistryConfiguration::Release:
                    spec.EnableValidation = true;
                    spec.EnableConflictDetection = false;
                    spec.EnableSizeValidation = false;
                    spec.EnableLifecycleTracking = false;
                    spec.EnableDebugInterface = false;
                    spec.EnablePerformanceMetrics = false;
                    spec.EnableResourceProfiling = false;
                    spec.EnableInvalidationTracking = true;
                    spec.EnableDependencyTracking = false;
                    break;
                    
                case RegistryConfiguration::Performance:
                    spec.EnableValidation = false;
                    spec.EnableConflictDetection = false;
                    spec.EnableSizeValidation = false;
                    spec.EnableLifecycleTracking = false;
                    spec.EnableDebugInterface = false;
                    spec.EnablePerformanceMetrics = false;
                    spec.EnableResourceProfiling = false;
                    spec.EnableInvalidationTracking = false;
                    spec.EnableDependencyTracking = false;
                    spec.EnableAutoOptimization = true;
                    break;
                    
                case RegistryConfiguration::Development:
                default:
                    spec.EnableValidation = true;
                    spec.EnableConflictDetection = true;
                    spec.EnableSizeValidation = true;
                    spec.EnableLifecycleTracking = true;
                    spec.EnableDebugInterface = true;
                    spec.EnablePerformanceMetrics = true;
                    spec.EnableResourceProfiling = false;
                    spec.EnableInvalidationTracking = true;
                    spec.EnableDependencyTracking = true;
                    break;
            }
            
            return spec;
        }
    };

    /**
     * @brief Information about a shader resource binding discovered through SPIR-V reflection
     */
    struct ShaderResourceBinding
    {
        ShaderResourceType Type = ShaderResourceType::None;
        u32 BindingPoint = 0;       // OpenGL binding point
        u32 Set = 0;                // Vulkan descriptor set (for future compatibility)
        std::string Name;           // Resource name in shader
        size_t Size = 0;             // Size for buffers, 0 for textures
        bool IsActive = false;      // Whether this resource is currently bound
        
        // Array support (Phase 1.2)
        bool IsArray = false;       // Whether this is an array resource
        u32 ArraySize = 0;          // Number of elements in the array (0 if not array)
        u32 BaseBindingPoint = 0;   // Base binding point for arrays
        
        // Phase 1.1: Resource Handle Tracking
        void* GPUHandle = nullptr;  // OpenGL buffer/texture ID cast to void*
        u32 LastBindFrame = 0;      // Frame number when last bound (for debugging)
        bool IsDirty = true;        // Resource needs rebinding optimization
        
        // Phase 3.1: Multi-set management - use existing Set field for descriptor set index

        ShaderResourceBinding() = default;
        ShaderResourceBinding(ShaderResourceType type, u32 bindingPoint, u32 set, 
                            const std::string& name, size_t size = 0)
            : Type(type), BindingPoint(bindingPoint), Set(set), Name(name), Size(size), IsActive(false)
        {}
        
        // Constructor for array resources
        ShaderResourceBinding(ShaderResourceType type, u32 baseBindingPoint, u32 set, 
                            const std::string& name, u32 arraySize, size_t elementSize = 0)
            : Type(type), BindingPoint(baseBindingPoint), Set(set), Name(name), 
              Size(elementSize), IsActive(false), IsArray(true), ArraySize(arraySize), 
              BaseBindingPoint(baseBindingPoint)
        {}
        
        // Phase 1.1: Helper methods for GPU handle management
        u32 GetOpenGLHandle() const { return static_cast<u32>(reinterpret_cast<uintptr_t>(GPUHandle)); }
        void SetOpenGLHandle(u32 handle) { GPUHandle = reinterpret_cast<void*>(static_cast<uintptr_t>(handle)); }
        
        // Dirty state management
        void MarkDirty() { IsDirty = true; }
        void MarkClean() { IsDirty = false; }
        bool IsDirtyState() const { return IsDirty; }
        
        // Frame tracking for debugging
        void UpdateBindFrame(u32 frameNumber) { LastBindFrame = frameNumber; IsActive = true; MarkClean(); }
        
        // Additional methods expected by BindingStateCache
        bool IsValid() const { return Type != ShaderResourceType::None && GPUHandle != nullptr; }
        u32 GetBufferHandle() const { return GetOpenGLHandle(); }
        GLintptr GetOffset() const { return 0; } // For basic buffer binding, offset is 0
        GLsizeiptr GetSize() const { return static_cast<GLsizeiptr>(Size); }
        ShaderResourceType GetResourceType() const { return Type; }
    };

    /**
     * @brief Variant type holding different shader resource types
     */
    using ShaderResource = std::variant<
        std::monostate,                         // Empty state
        Ref<UniformBuffer>,                     // Uniform buffer
        Ref<StorageBuffer>,                     // Storage buffer (SSBO)
        Ref<Texture2D>,                         // 2D texture
        Ref<TextureCubemap>,                    // Cubemap texture
        // Array resource types (Phase 1.2)
        Ref<UniformBufferArray>,                // Array of uniform buffers
        Ref<StorageBufferArray>,                // Array of storage buffers
        Ref<Texture2DArray>,                    // Array of 2D textures
        Ref<TextureCubemapArray>                // Array of cubemap textures
        // Note: Image2D will be added in future updates
    >;

    /**
     * @brief Enhanced input structure for setting shader resources with Hazel-style indexed support
     */
    struct ShaderResourceInput
    {
        ShaderResourceType Type;
        ShaderResource Resource;
        
        // Enhanced features for indexed resources (Hazel-style)
        u32 Set = 0;                    // Descriptor set index (for organization)
        u32 Binding = UINT32_MAX;       // Explicit binding point override
        u32 ArrayIndex = 0;             // Index within array resources
        bool IsIndexed = false;         // Whether this is an indexed access
        bool HasExplicitBinding = false; // Whether binding was explicitly set
        
        // Resource metadata for advanced features
        std::string DebugName;          // Optional debug name
        bool IsOptional = false;        // Whether resource is optional
        UpdatePriority Priority = UpdatePriority::Normal; // Update priority
        
        // Constructors for type-safe resource setting
        ShaderResourceInput() : Type(ShaderResourceType::None), Resource(std::monostate{}) {}

        explicit ShaderResourceInput(const Ref<UniformBuffer>& buffer)
            : Type(ShaderResourceType::UniformBuffer), Resource(buffer) {}

        explicit ShaderResourceInput(const Ref<StorageBuffer>& buffer)
            : Type(ShaderResourceType::StorageBuffer), Resource(buffer) {}

        explicit ShaderResourceInput(const Ref<Texture2D>& texture)
            : Type(ShaderResourceType::Texture2D), Resource(texture) {}

        explicit ShaderResourceInput(const Ref<TextureCubemap>& texture)
            : Type(ShaderResourceType::TextureCube), Resource(texture) {}

        // Array resource constructors (Phase 1.2)
        explicit ShaderResourceInput(const Ref<UniformBufferArray>& array)
            : Type(ShaderResourceType::UniformBufferArray), Resource(array) {}

        explicit ShaderResourceInput(const Ref<StorageBufferArray>& array)
            : Type(ShaderResourceType::StorageBufferArray), Resource(array) {}

        explicit ShaderResourceInput(const Ref<Texture2DArray>& array)
            : Type(ShaderResourceType::Texture2DArray), Resource(array) {}

        explicit ShaderResourceInput(const Ref<TextureCubemapArray>& array)
            : Type(ShaderResourceType::TextureCubeArray), Resource(array) {}

        // Enhanced constructors with indexed access (Hazel-style)
        ShaderResourceInput(const Ref<UniformBuffer>& buffer, u32 set, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::UniformBuffer), Resource(buffer), Set(set), Binding(binding),
              HasExplicitBinding(binding != UINT32_MAX) {}

        ShaderResourceInput(const Ref<StorageBuffer>& buffer, u32 set, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::StorageBuffer), Resource(buffer), Set(set), Binding(binding),
              HasExplicitBinding(binding != UINT32_MAX) {}

        ShaderResourceInput(const Ref<Texture2D>& texture, u32 set, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::Texture2D), Resource(texture), Set(set), Binding(binding),
              HasExplicitBinding(binding != UINT32_MAX) {}

        ShaderResourceInput(const Ref<TextureCubemap>& texture, u32 set, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::TextureCube), Resource(texture), Set(set), Binding(binding),
              HasExplicitBinding(binding != UINT32_MAX) {}

        // Array resource constructors with indexing
        ShaderResourceInput(const Ref<UniformBufferArray>& array, u32 set, u32 arrayIndex = 0, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::UniformBufferArray), Resource(array), Set(set), Binding(binding),
              ArrayIndex(arrayIndex), IsIndexed(true), HasExplicitBinding(binding != UINT32_MAX) {}

        ShaderResourceInput(const Ref<StorageBufferArray>& array, u32 set, u32 arrayIndex = 0, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::StorageBufferArray), Resource(array), Set(set), Binding(binding),
              ArrayIndex(arrayIndex), IsIndexed(true), HasExplicitBinding(binding != UINT32_MAX) {}

        ShaderResourceInput(const Ref<Texture2DArray>& array, u32 set, u32 arrayIndex = 0, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::Texture2DArray), Resource(array), Set(set), Binding(binding),
              ArrayIndex(arrayIndex), IsIndexed(true), HasExplicitBinding(binding != UINT32_MAX) {}

        ShaderResourceInput(const Ref<TextureCubemapArray>& array, u32 set, u32 arrayIndex = 0, u32 binding = UINT32_MAX)
            : Type(ShaderResourceType::TextureCubeArray), Resource(array), Set(set), Binding(binding),
              ArrayIndex(arrayIndex), IsIndexed(true), HasExplicitBinding(binding != UINT32_MAX) {}

        // Fluent interface for configuration
        ShaderResourceInput& WithSet(u32 set) { Set = set; return *this; }
        ShaderResourceInput& WithBinding(u32 binding) { Binding = binding; HasExplicitBinding = true; return *this; }
        ShaderResourceInput& WithArrayIndex(u32 index) { ArrayIndex = index; IsIndexed = true; return *this; }
        ShaderResourceInput& WithDebugName(const std::string& name) { DebugName = name; return *this; }
        ShaderResourceInput& AsOptional(bool optional = true) { IsOptional = optional; return *this; }
        ShaderResourceInput& WithPriority(UpdatePriority priority) { Priority = priority; return *this; }
    };

    /**
     * @brief Phase 3.2: Information structure for default resource creation
     */
    struct ShaderResourceInfo
    {
        std::string Name;
        ShaderResourceType Type = ShaderResourceType::None;
        u32 Size = 0;              // Size in bytes (for buffers)
        u32 Binding = UINT32_MAX;  // Binding point
        u32 Set = UINT32_MAX;      // Descriptor set index (Phase 3.1)
        bool IsArray = false;      // Whether this is an array resource
        u32 ArraySize = 1;         // Array size if IsArray is true
        
        ShaderResourceInfo() = default;
        ShaderResourceInfo(const std::string& name, ShaderResourceType type, u32 binding)
            : Name(name), Type(type), Binding(binding) {}
    };

    /**
     * @brief Phase 4.1: Detailed invalidation tracking information
     */
    struct InvalidationInfo
    {
        std::string ResourceName;
        InvalidationReason Reason = InvalidationReason::Unknown;
        u32 BindingPoint = UINT32_MAX;
        u32 FrameInvalidated = 0;
        std::chrono::steady_clock::time_point Timestamp;
        std::vector<std::string> Dependencies;  // Resources that depend on this one
        std::vector<std::string> Dependents;    // Resources this one depends on
        
        InvalidationInfo() = default;
        InvalidationInfo(const std::string& name, InvalidationReason reason, u32 binding)
            : ResourceName(name), Reason(reason), BindingPoint(binding), 
              Timestamp(std::chrono::steady_clock::now()) {}
    };

    /**
     * @brief Phase 4.2: Update batch information for efficient processing
     */
    struct UpdateBatch
    {
        ShaderResourceType ResourceType;
        UpdatePriority Priority = UpdatePriority::Normal;
        std::vector<std::string> ResourceNames;
        u32 FrameScheduled = 0;
        u32 EstimatedCost = 0;  // Estimated GPU state change cost
        bool IsProcessed = false;
        
        UpdateBatch() = default;
        UpdateBatch(ShaderResourceType type, UpdatePriority priority)
            : ResourceType(type), Priority(priority) {}
    };

    /**
     * @brief Phase 4.2: Update statistics for profiling and optimization
     */
    struct UpdateStatistics
    {
        u32 TotalUpdates = 0;
        u32 BatchedUpdates = 0;
        u32 ImmediateUpdates = 0;
        u32 DeferredUpdates = 0;
        f32 AverageUpdateTime = 0.0f;
        f32 AverageBatchSize = 0.0f;
        u32 StateChangeSavings = 0;  // Number of state changes saved through batching
        
        void Reset()
        {
            TotalUpdates = BatchedUpdates = ImmediateUpdates = DeferredUpdates = 0;
            AverageUpdateTime = AverageBatchSize = 0.0f;
            StateChangeSavings = 0;
        }
    };

    /**
     * @brief Phase 5.1: Comprehensive resource declaration similar to Hazel's RenderPassInputDeclaration
     */
    struct ResourceDeclaration
    {
        std::string Name;
        ShaderResourceType Type = ShaderResourceType::None;
        u32 BindingPoint = UINT32_MAX;
        u32 Set = UINT32_MAX;
        u32 Size = 0;                    // Size in bytes for buffers
        u32 ArraySize = 1;               // Array size (1 for non-arrays)
        bool IsArray = false;
        bool IsOptional = false;         // Whether this resource is optional
        bool IsWritable = false;         // Whether resource can be written to
        ResourceUsagePattern UsagePattern = ResourceUsagePattern::Unknown;
        
        // SPIR-V reflection metadata
        struct SPIRVMetadata
        {
            u32 TypeID = 0;              // SPIR-V type ID
            u32 BaseTypeID = 0;          // Base type ID for structs
            std::vector<u32> MemberTypes; // Member type IDs for structs
            std::vector<std::string> MemberNames; // Member names for structs
            std::vector<u32> MemberOffsets; // Member offsets in bytes
            u32 Stride = 0;              // Stride for arrays/matrices
            bool HasDecorations = false;  // Whether type has decorations
        } SPIRVInfo;
        
        // Usage statistics
        struct UsageStatistics
        {
            u32 ReadCount = 0;           // Number of times read
            u32 WriteCount = 0;          // Number of times written
            u32 BindCount = 0;           // Number of times bound
            f32 AverageUpdateFrequency = 0.0f; // Updates per frame
            std::chrono::steady_clock::time_point LastAccessed;
            std::chrono::steady_clock::time_point FirstAccessed;
            u64 TotalSizeTransferred = 0; // Total bytes transferred
        } Statistics;
        
        ResourceDeclaration() = default;
        ResourceDeclaration(const std::string& name, ShaderResourceType type, u32 binding)
            : Name(name), Type(type), BindingPoint(binding) {}
    };

    /**
     * @brief Phase 5.2: Validation issue tracking with severity levels
     */
    struct RegistryValidationIssue
    {
        RegistryValidationSeverity Severity = RegistryValidationSeverity::Warning;
        std::string Category;         // e.g., "BindingConflict", "SizeAlignment", "Lifecycle"
        std::string ResourceName;     // Resource that caused the issue
        std::string Message;          // Human-readable description
        std::string Suggestion;       // Suggested fix
        u32 BindingPoint = UINT32_MAX; // Relevant binding point
        u32 Line = 0;                 // Source line (if available)
        std::chrono::steady_clock::time_point Timestamp;
        
        RegistryValidationIssue() = default;
        RegistryValidationIssue(RegistryValidationSeverity severity, const std::string& category, 
                               const std::string& resource, const std::string& message)
            : Severity(severity), Category(category), ResourceName(resource), 
              Message(message), Timestamp(std::chrono::steady_clock::now()) {}
    };

    /**
     * @brief Phase 5.2: Resource lifecycle state tracking
     */
    enum class ResourceLifecycleState : u8
    {
        Unknown = 0,
        Declared,        // Resource discovered through reflection
        Allocated,       // Resource memory allocated
        Bound,           // Resource bound to pipeline
        Active,          // Resource actively being used
        Stale,           // Resource outdated but still bound
        Unbound,         // Resource unbound from pipeline
        Deallocated,     // Resource memory freed
        Destroyed        // Resource completely destroyed
    };

    /**
     * @brief Phase 5.2: Enhanced lifecycle tracking information
     */
    struct ResourceLifecycleInfo
    {
        ResourceLifecycleState State = ResourceLifecycleState::Unknown;
        std::chrono::steady_clock::time_point StateChanged;
        std::chrono::steady_clock::time_point Created;
        std::chrono::steady_clock::time_point LastBound;
        std::chrono::steady_clock::time_point LastUnbound;
        u32 BindCount = 0;
        u32 UnbindCount = 0;
        bool IsValid = true;         // Whether resource is in valid state
        std::string LastError;       // Last error message if any
        
        ResourceLifecycleInfo() 
            : StateChanged(std::chrono::steady_clock::now()),
              Created(std::chrono::steady_clock::now()) {}
    };

    /**
     * @brief Registry for managing shader resources with automatic discovery and binding
     * 
     * This class provides a centralized system for managing shader resources similar to
     * Hazel's DescriptorSetManager but adapted for OpenGL. It automatically discovers
     * resources through SPIR-V reflection and provides type-safe resource binding.
     */
    class UniformBufferRegistry
    {
    public:
        UniformBufferRegistry() = default;
        explicit UniformBufferRegistry(const Ref<Shader>& shader);
        
        // Phase 2.2: Specification-based constructor
        explicit UniformBufferRegistry(const Ref<Shader>& shader, const UniformBufferRegistrySpecification& spec);
        
        ~UniformBufferRegistry();

        // Copy semantics for template/cloning support
        UniformBufferRegistry(const UniformBufferRegistry& other);
        UniformBufferRegistry& operator=(const UniformBufferRegistry& other);

        // Move semantics
        UniformBufferRegistry(UniformBufferRegistry&&) = default;
        UniformBufferRegistry& operator=(UniformBufferRegistry&&) = default;

        // Phase 2.1: Template and Clone Support
        
        /**
         * @brief Create a template registry from an existing registry configuration
         * @param templateRegistry Source registry to create template from
         * @param templateName Name for the template
         * @return Unique pointer to template registry
         */
        static Scope<UniformBufferRegistry> CreateTemplate(const UniformBufferRegistry& templateRegistry, 
                                                          const std::string& templateName = "");

        /**
         * @brief Clone this registry for use with a different shader
         * @param targetShader Shader to associate with the cloned registry
         * @param cloneName Optional name for the cloned registry
         * @return Unique pointer to cloned registry
         */
        Scope<UniformBufferRegistry> Clone(const Ref<Shader>& targetShader, 
                                         const std::string& cloneName = "") const;

        /**
         * @brief Create a registry instance from a template
         * @param templateRegistry Template registry to clone from
         * @param targetShader Shader to associate with the new registry
         * @param instanceName Name for the instance
         * @return Unique pointer to registry instance
         */
        static Scope<UniformBufferRegistry> CreateFromTemplate(const UniformBufferRegistry& templateRegistry,
                                                              const Ref<Shader>& targetShader,
                                                              const std::string& instanceName = "");

        /**
         * @brief Validate template compatibility with target shader
         * @param targetShader Shader to validate compatibility with
         * @return true if template is compatible, false otherwise
         */
        bool ValidateTemplateCompatibility(const Ref<Shader>& targetShader) const;

        /**
         * @brief Get the specification used to create this registry
         * @return Current registry specification
         */
        const UniformBufferRegistrySpecification& GetSpecification() const { return m_Specification; }

        /**
         * @brief Update registry specification (some settings require reinitialization)
         * @param newSpec New specification to apply
         * @param reinitialize Whether to reinitialize the registry with new settings
         */
        void UpdateSpecification(const UniformBufferRegistrySpecification& newSpec, bool reinitialize = false);

        // Phase 3.1: Multi-Set Management
        
        /**
         * @brief Configure descriptor set for a specific priority level
         * @param priority Priority level for the descriptor set
         * @param setIndex Set index to assign to this priority
         * @param name Optional name for the descriptor set
         */
        void ConfigureDescriptorSet(DescriptorSetPriority priority, u32 setIndex, const std::string& name = "");

        /**
         * @brief Assign a resource to a specific descriptor set
         * @param resourceName Name of the resource to assign
         * @param setIndex Descriptor set index to assign to
         * @return true if assignment was successful
         */
        bool AssignResourceToSet(const std::string& resourceName, u32 setIndex);

        /**
         * @brief Automatically assign resources to appropriate descriptor sets based on priority
         * @param useHeuristics Whether to use smart heuristics for assignment
         */
        void AutoAssignResourceSets(bool useHeuristics = true);

        /**
         * @brief Get descriptor set information for a specific set index
         * @param setIndex Index of the descriptor set
         * @return Pointer to descriptor set info, or nullptr if not found
         */
        const DescriptorSetInfo* GetDescriptorSetInfo(u32 setIndex) const;

        /**
         * @brief Get all configured descriptor sets
         * @return Map of set index to descriptor set info
         */
        const std::unordered_map<u32, DescriptorSetInfo>& GetDescriptorSets() const { return m_DescriptorSets; }

        /**
         * @brief Get the descriptor set index for a resource
         * @param resourceName Name of the resource
         * @return Set index, or UINT32_MAX if not assigned
         */
        u32 GetResourceSetIndex(const std::string& resourceName) const;

        /**
         * @brief Bind resources for a specific descriptor set only
         * @param setIndex Descriptor set index to bind
         */
        void BindDescriptorSet(u32 setIndex);

        /**
         * @brief Bind all descriptor sets in priority order
         */
        void BindAllSets();

        /**
         * @brief Get binding order based on set priorities
         * @return Vector of set indices in optimal binding order
         */
        const std::vector<u32>& GetSetBindingOrder() const { return m_SetBindingOrder; }

        // Phase 3.2: Default Resource System
        
        /**
         * @brief Initialize default resources based on common shader patterns
         * @param forceReinitialize Whether to reinitialize if already initialized
         */
        void InitializeDefaultResources(bool forceReinitialize = false);

        /**
         * @brief Add a default resource template
         * @param resourceName Name of the default resource
         * @param resourceInfo Information about the default resource
         */
        void AddDefaultResource(const std::string& resourceName, const ShaderResourceInfo& resourceInfo);

        /**
         * @brief Create common system resources (view/projection matrices, time, etc.)
         */
        void CreateSystemDefaults();

        /**
         * @brief Create common material resources based on detected shader pattern
         */
        void CreateMaterialDefaults();

        /**
         * @brief Get default resource template library
         * @return Map of template names to registry specifications
         */
        const std::unordered_map<std::string, UniformBufferRegistrySpecification>& GetResourceTemplates() const 
        { 
            return m_ResourceTemplates; 
        }

        /**
         * @brief Apply a resource template to this registry
         * @param templateName Name of the template to apply
         * @return true if template was found and applied successfully
         */
        bool ApplyResourceTemplate(const std::string& templateName);

        /**
         * @brief Detect shader pattern and suggest appropriate defaults
         * @return Suggested resource template name, or empty string if no pattern detected
         */
        std::string DetectShaderPattern() const;

        /**
         * @brief Initialize the registry and discover resources from associated shader
         */
        void Initialize();

        /**
         * @brief Set the associated shader for this registry
         * @param shader Shader to associate with this registry
         */
        void SetShader(const Ref<Shader>& shader) { m_Shader = shader; }

        /**
         * @brief Shutdown the registry and clear all resources
         */
        void Shutdown();

        /**
         * @brief Discover shader resources from SPIR-V reflection data
         * @param stage Shader stage (GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, etc.)
         * @param spirvData SPIR-V bytecode for reflection
         */
        void DiscoverResources(u32 stage, const std::vector<u32>& spirvData);

        /**
         * @brief Enhanced SPIR-V discovery that populates OpenGL declarations
         * @param stage Shader stage (GL_VERTEX_SHADER, etc.)
         * @param spirvData SPIR-V bytecode for reflection
         * @param passName Name of the shader pass for declaration context
         * @return True if discovery was successful
         */
        bool DiscoverResourcesEnhanced(u32 stage, const std::vector<u32>& spirvData, const std::string& passName = "");

        /**
         * @brief Get or create OpenGL resource declaration for this registry
         * @param passName Name of the shader pass (auto-generated if empty)
         * @return Reference to OpenGL resource declaration
         */
        OpenGLResourceDeclaration& GetOpenGLDeclaration(const std::string& passName = "");

        /**
         * @brief Get existing OpenGL resource declaration (const version)
         * @param passName Name of the shader pass
         * @return Pointer to declaration or nullptr if not found
         */
        const OpenGLResourceDeclaration* GetOpenGLDeclaration(const std::string& passName) const;

        /**
         * @brief Synchronize traditional registry with OpenGL declarations
         * @param declaration OpenGL declaration to sync from
         * @return True if synchronization was successful
         */
        bool SynchronizeWithDeclaration(const OpenGLResourceDeclaration& declaration);

        /**
         * @brief Export registry data to OpenGL declaration format
         * @param passName Name for the exported declaration
         * @return OpenGL resource declaration
         */
        OpenGLResourceDeclaration ExportToDeclaration(const std::string& passName) const;

        /**
         * @brief Set a shader resource by name with type-safe input
         * @param name Resource name as defined in shader
         * @param input Type-safe resource input
         * @return true if resource was set successfully, false otherwise
         */
        bool SetResource(const std::string& name, const ShaderResourceInput& input);

        /**
         * @brief Template method for setting resources with automatic type deduction
         * @tparam T Resource type (UniformBuffer, Texture2D, etc.)
         * @param name Resource name as defined in shader
         * @param resource Resource to bind
         * @return true if resource was set successfully, false otherwise
         */
        template<typename T>
        bool SetResource(const std::string& name, const Ref<T>& resource)
        {
            return SetResource(name, ShaderResourceInput(resource));
        }

        /**
         * @brief Create and bind an array resource
         * @tparam T Base resource type (StorageBuffer, Texture2D, etc.)
         * @param name Array resource name as defined in shader
         * @param baseBindingPoint Starting binding point for the array
         * @param maxSize Maximum number of resources in the array
         * @return Shared pointer to the created array resource
         */
        template<typename T>
        Ref<ArrayResource<T>> CreateArrayResource(const std::string& name, u32 baseBindingPoint, u32 maxSize = 32)
        {
            auto arrayResource = CreateRef<ArrayResource<T>>(name, baseBindingPoint, maxSize);
            
            // Create the appropriate variant type and bind it
            if constexpr (std::is_same_v<T, StorageBuffer>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<StorageBufferArray>(arrayResource)));
            }
            else if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<UniformBufferArray>(arrayResource)));
            }
            else if constexpr (std::is_same_v<T, Texture2D>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<Texture2DArray>(arrayResource)));
            }
            else if constexpr (std::is_same_v<T, TextureCubemap>)
            {
                SetResource(name, ShaderResourceInput(std::static_pointer_cast<TextureCubemapArray>(arrayResource)));
            }
            
            return arrayResource;
        }

        /**
         * @brief Get an array resource by name
         * @tparam T Base resource type (StorageBuffer, Texture2D, etc.)
         * @param name Array resource name
         * @return Array resource if found and type matches, nullptr otherwise
         */
        template<typename T>
        Ref<ArrayResource<T>> GetArrayResource(const std::string& name) const
        {
            auto it = m_BoundResources.find(name);
            if (it == m_BoundResources.end())
                return nullptr;

            if constexpr (std::is_same_v<T, StorageBuffer>)
            {
                if (std::holds_alternative<Ref<StorageBufferArray>>(it->second))
                    return std::get<Ref<StorageBufferArray>>(it->second);
            }
            else if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                if (std::holds_alternative<Ref<UniformBufferArray>>(it->second))
                    return std::get<Ref<UniformBufferArray>>(it->second);
            }
            else if constexpr (std::is_same_v<T, Texture2D>)
            {
                if (std::holds_alternative<Ref<Texture2DArray>>(it->second))
                    return std::get<Ref<Texture2DArray>>(it->second);
            }
            else if constexpr (std::is_same_v<T, TextureCubemap>)
            {
                if (std::holds_alternative<Ref<TextureCubemapArray>>(it->second))
                    return std::get<Ref<TextureCubemapArray>>(it->second);
            }

            return nullptr;
        }

        /**
         * @brief Get a bound resource by name (legacy method)
         * @tparam T Expected resource type
         * @param name Resource name as defined in shader
         * @return Resource reference if found and type matches, nullptr otherwise
         */
        template<typename T>
        Ref<T> GetResource(const std::string& name) const
        {
            auto it = m_BoundResources.find(name);
            if (it == m_BoundResources.end())
                return nullptr;

            if (std::holds_alternative<Ref<T>>(it->second))
                return std::get<Ref<T>>(it->second);

            return nullptr;
        }

        // ==========================================
        // Phase 6: Enhanced Resource Access
        // ==========================================

        /**
         * @brief Enhanced resource getter with comprehensive error handling and smart conversions
         * @tparam T Expected resource type
         * @param name Resource name as defined in shader
         * @return Resource access result with detailed error information
         */
        template<typename T>
        ResourceAccessResult<T> GetResourceEnhanced(const std::string& name) const;  // Implementation in .cpp file

        /**
         * @brief Get resource with automatic fallback
         * @tparam T Expected resource type
         * @param name Resource name
         * @param fallback Fallback resource if not found or invalid
         * @return Resource or fallback
         */
        template<typename T>
        Ref<T> GetResourceOrFallback(const std::string& name, Ref<T> fallback) const
        {
            auto resource = GetResource<T>(name);
            return resource ? resource : fallback;
        }

        /**
         * @brief Get resource with factory function for missing resources
         * @tparam T Expected resource type
         * @param name Resource name
         * @param factory Factory function to create resource if missing
         * @return Resource (existing or newly created)
         */
        template<typename T>
        Ref<T> GetOrCreateResource(const std::string& name, std::function<Ref<T>()> factory)
        {
            auto resource = GetResource<T>(name);
            if (resource)
                return resource;
            
            if (factory)
            {
                auto newResource = factory();
                if (newResource)
                {
                    // Note: This is a simplified implementation
                    // Full implementation would need to properly register the resource
                    return newResource;
                }
            }
            return nullptr;
        }

        /**
         * @brief Check if resource is available and ready to use
         * @tparam T Expected resource type
         * @param name Resource name
         * @return True if resource is available and valid
         */
        template<typename T>
        bool IsResourceReady(const std::string& name) const
        {
            auto resource = GetResource<T>(name);
            return resource != nullptr;
        }

        /**
         * @brief Get cached GPU handle for a resource
         * @param name Resource name
         * @return GPU handle or 0 if not cached/found
         */
        u32 GetCachedHandle(const std::string& name) const;

        /**
         * @brief Invalidate cached handle when resource changes
         * @param name Resource name
         */
        void InvalidateCachedHandle(const std::string& name);

        /**
         * @brief Get handle pool for temporary resources
         * @tparam T Resource type
         * @return Handle pool or nullptr if not available
         */
        template<typename T>
        void* GetHandlePool()
        {
            // Basic implementation - returns nullptr for now
            return nullptr;
        }

        /**
         * @brief Create handle pool for a resource type
         * @tparam T Resource type
         * @param maxSize Maximum pool size
         * @param factory Factory function for creating resources
         */
        template<typename T>
        void CreateHandlePool(u32 maxSize, std::function<Ref<T>()> factory)
        {
            // Basic implementation - not implemented yet
            OLO_CORE_WARN("CreateHandlePool not implemented yet for type");
        }

        /**
         * @brief Enable/disable resource handle caching
         * @param enabled Whether to enable handle caching
         */
        void SetHandleCachingEnabled(bool enabled);

        /**
         * @brief Get resource binding information (const access)
         * @param name Resource name
         * @return Resource binding or nullptr if not found
         */
        const ShaderResourceBinding* GetResourceBinding(const std::string& name) const;

        // Enhanced template methods now use inline implementations above
        // Note: Additional template specializations can be added in the .cpp file if needed

        // ==========================================
        // Step 11: Direct State Access (DSA) Integration
        // ==========================================

        /**
         * @brief Get the DSA binding manager
         * @return Reference to the DSA binding manager
         */
        DSABindingManager& GetDSABindingManager() { return *m_DSABindingManager; }
        const DSABindingManager& GetDSABindingManager() const { return *m_DSABindingManager; }

        /**
         * @brief Check if DSA features are available and enabled
         * @return True if DSA can be used for efficient binding
         */
        bool IsDSAEnabled() const;

        /**
         * @brief Apply all bound resources using DSA for maximum efficiency
         * This uses OpenGL 4.5+ Direct State Access features when available
         * @param enableBatching Whether to use batch binding operations
         * @return Number of resources successfully bound
         */
        u32 ApplyBindingsWithDSA(bool enableBatching = true);

        /**
         * @brief Apply bindings for a specific descriptor set using DSA
         * @param setIndex Descriptor set index to apply
         * @param enableBatching Whether to use batch binding operations
         * @return Number of resources successfully bound
         */
        u32 ApplySetBindingsWithDSA(u32 setIndex, bool enableBatching = true);

        /**
         * @brief Enable or disable DSA usage (if supported)
         * @param enabled Whether to use DSA for bindings
         */
        void SetDSAEnabled(bool enabled);

        /**
         * @brief Get DSA binding statistics for performance monitoring
         * @return DSA statistics structure
         */
        struct DSAStatistics GetDSAStatistics() const;

        /**
         * @brief Reset DSA binding statistics
         */
        void ResetDSAStatistics();

        /**
         * @brief Invalidate all DSA binding state (forces rebind on next application)
         */
        void InvalidateDSABindings();

        // ==========================================
        // Step 12: Binding State Caching Integration
        // ==========================================

        /**
         * @brief Check if binding state caching is enabled
         * @return True if cache is active and functional
         */
        bool IsCacheEnabled() const { return m_CacheEnabled && m_CacheInitialized; }

        /**
         * @brief Enable or disable binding state caching
         * @param enabled Whether to enable caching
         * @param policy Cache policy to use
         * @param strategy Invalidation strategy
         * @return True if cache state was changed successfully
         */
        bool SetCacheEnabled(bool enabled, 
                           BindingStateCache::CachePolicy policy = BindingStateCache::CachePolicy::Balanced,
                           BindingStateCache::InvalidationStrategy strategy = BindingStateCache::InvalidationStrategy::FrameBased);

        /**
         * @brief Apply all bound resources with intelligent caching
         * This leverages the binding state cache to avoid redundant OpenGL calls
         * @param forceRebind Whether to force rebinding even if cached
         * @return Number of resources that were actually bound (not cached)
         */
        u32 ApplyBindingsWithCache(bool forceRebind = false);

        /**
         * @brief Apply bindings for a specific descriptor set with caching
         * @param setIndex Descriptor set index to apply
         * @param forceRebind Whether to force rebinding even if cached
         * @return Number of resources that were actually bound (not cached)
         */
        u32 ApplySetBindingsWithCache(u32 setIndex, bool forceRebind = false);

        /**
         * @brief Apply bindings using both DSA and caching for maximum efficiency
         * Combines OpenGL 4.5+ DSA features with intelligent binding state caching
         * @param enableBatching Whether to use DSA batch operations
         * @param forceRebind Whether to force rebinding even if cached
         * @return Number of resources that were actually bound
         */
        u32 ApplyBindingsOptimal(bool enableBatching = true, bool forceRebind = false);

        /**
         * @brief Invalidate binding cache for this registry
         * @param resourceType Optional resource type to invalidate (all if not specified)
         */
        void InvalidateCache(ShaderResourceType resourceType = ShaderResourceType::None);

        /**
         * @brief Update frame counter for frame-based cache invalidation
         * @param frameNumber Current frame number
         */
        void SetCurrentFrame(u32 frameNumber);

        /**
         * @brief Get binding state cache performance statistics
         * @return Current cache statistics
         */
        BindingCacheStatistics GetCacheStatistics() const;

        /**
         * @brief Get cache information (size, memory usage, etc.)
         * @return Cache information structure
         */
        BindingStateCache::CacheInfo GetCacheInfo() const;

        /**
         * @brief Clean up stale cached bindings
         * @param maxAge Maximum age before considering binding stale
         * @return Number of stale bindings removed
         */
        u32 CleanupStaleCache(std::chrono::milliseconds maxAge = std::chrono::milliseconds(10000));

        /**
         * @brief Validate cache against actual OpenGL state
         * @param fullValidation Whether to perform full validation or sampling
         * @return True if cache is accurate
         */
        bool ValidateCache(bool fullValidation = false);

        /**
         * @brief Synchronize cache with current OpenGL state
         * Clears cache and rebuilds from actual OpenGL state
         */
        void SynchronizeCache();

        /**
         * @brief Apply all bound resources to OpenGL state
         * This performs the actual OpenGL binding calls using the descriptor set system
         */
        void ApplyBindings();

        /**
         * @brief Apply bindings for a specific descriptor set only
         * @param setIndex Descriptor set index to apply
         */
        void ApplySetBindings(u32 setIndex);

        /**
         * @brief Get the OpenGL descriptor set manager
         * @return Reference to the descriptor set manager
         */
        OpenGLDescriptorSetManager& GetDescriptorSetManager() { return *m_DescriptorSetManager; }
        const OpenGLDescriptorSetManager& GetDescriptorSetManager() const { return *m_DescriptorSetManager; }

        /**
         * @brief Validate that all required resources are bound
         * @return true if all required resources are bound, false otherwise
         */
        bool Validate() const;

        /**
         * @brief Check if a resource is bound by name
         * @param name Resource name to check
         * @return true if resource is bound, false otherwise
         */
        bool IsResourceBound(const std::string& name) const;

        /**
         * @brief Get binding information for a resource by name
         * @param name Resource name
         * @return Pointer to binding info if found, nullptr otherwise
         */
        const ShaderResourceBinding* GetBindingInfo(const std::string& name) const;

        /**
         * @brief Get all discovered resource bindings
         * @return Reference to the bindings map
         */
        const std::unordered_map<std::string, ShaderResourceBinding>& GetBindings() const { return m_ResourceBindings; }

        /**
         * @brief Get all bound resources
         * @return Reference to the bound resources map
         */
        const std::unordered_map<std::string, ShaderResource>& GetBoundResources() const { return m_BoundResources; }

        /**
         * @brief Get the associated shader
         * @return Shader reference
         */
        Ref<Shader> GetShader() const { return m_Shader; }

        // Frame-in-flight management (Phase 1.3)
        
        /**
         * @brief Enable frame-in-flight resource management
         * @param framesInFlight Number of frames to keep in flight (default: 3)
         */
        void EnableFrameInFlight(u32 framesInFlight = 3);

        /**
         * @brief Disable frame-in-flight resource management
         */
        void DisableFrameInFlight();

        /**
         * @brief Check if frame-in-flight is enabled
         */
        bool IsFrameInFlightEnabled() const { return m_FrameInFlightEnabled; }

        /**
         * @brief Register a resource for frame-in-flight management
         * @param name Resource name
         * @param size Resource size (for buffers)
         * @param usage Buffer usage pattern
         * @param arraySize Array size (for array resources, 0 for single resources)
         * @param baseBindingPoint Base binding point (for array resources)
         */
        void RegisterFrameInFlightResource(const std::string& name, ShaderResourceType type, u32 size = 0, 
                                         BufferUsage usage = BufferUsage::Dynamic, u32 arraySize = 0, u32 baseBindingPoint = 0);

        /**
         * @brief Advance to the next frame (call at the beginning of each frame)
         */
        void NextFrame();

        /**
         * @brief Get current frame's resource
         * @tparam T Resource type
         * @param name Resource name
         * @return Current frame's resource
         */
        template<typename T>
        Ref<T> GetCurrentFrameResource(const std::string& name) const
        {
            if (!m_FrameInFlightEnabled || !m_FrameInFlightManager)
                return GetResource<T>(name);

            if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                return m_FrameInFlightManager->GetCurrentUniformBuffer(name);
            }
            else if constexpr (std::is_same_v<T, StorageBuffer>)
            {
                return m_FrameInFlightManager->GetCurrentStorageBuffer(name);
            }
            else if constexpr (std::is_same_v<T, UniformBufferArray>)
            {
                return m_FrameInFlightManager->GetCurrentUniformBufferArray(name);
            }
            else if constexpr (std::is_same_v<T, StorageBufferArray>)
            {
                return m_FrameInFlightManager->GetCurrentStorageBufferArray(name);
            }

            // Fallback to regular resource
            return GetResource<T>(name);
        }

        /**
         * @brief Get frame-in-flight manager statistics
         */
        FrameInFlightManager::Statistics GetFrameInFlightStatistics() const;

        /**
         * @brief Clear all bound resources but keep binding information
         */
        void ClearResources();

        /**
         * @brief Check if registry has any dirty bindings that need to be applied
         * @return true if there are dirty bindings, false otherwise
         */
        bool HasDirtyBindings() const { return !m_DirtyBindings.empty(); }

        /**
         * @brief Get statistics about the registry
         */
        struct Statistics
        {
            u32 TotalBindings = 0;
            u32 BoundResources = 0;
            u32 UniformBuffers = 0;
            u32 Textures = 0;
            u32 DirtyBindings = 0;
        };

        Statistics GetStatistics() const;

        // Debug support
        /**
         * @brief Get debug information about missing resources
         * @return Vector of names of missing required resources
         */
        std::vector<std::string> GetMissingResources() const;

        /**
         * @brief Render ImGui debug interface for this registry
         */
        void RenderDebugInterface();

    private:
        /**
         * @brief Validate resource type matches binding type
         * @param binding Binding information
         * @param input Resource input to validate
         * @return true if types match, false otherwise
         */
        bool ValidateResourceType(const ShaderResourceBinding& binding, const ShaderResourceInput& input) const;

        /**
         * @brief Apply a specific resource binding to OpenGL state
         * @param name Resource name
         * @param binding Binding information
         * @param resource Resource to bind
         */
        void ApplyResourceBinding(const std::string& name, const ShaderResourceBinding& binding, const ShaderResource& resource);

        /**
         * @brief Mark a binding as dirty for batched updates
         * @param name Resource name
         */
        void MarkBindingDirty(const std::string& name);

        // Phase 1.2: Two-Phase Resource Updates
        /**
         * @brief Invalidate a resource, marking it for pending update
         * @param name Resource name to invalidate
         */
        void InvalidateResource(const std::string& name);

        /**
         * @brief Commit all pending resource updates in an optimized batch
         */
        void CommitPendingUpdates();

        /**
         * @brief Check if a resource is currently invalidated and pending update
         * @param name Resource name to check
         * @return true if resource is invalidated, false otherwise
         */
        bool IsResourceInvalidated(const std::string& name) const;

        // ==========================================
        // Phase 4: Advanced Invalidation System
        // ==========================================

        // Phase 4.1: Granular Invalidation Tracking
        /**
         * @brief Invalidate a resource with detailed reason tracking and dependency propagation
         * @param name Resource name to invalidate
         * @param reason Reason for invalidation (for debugging and analytics)
         * @param propagateToDependents Whether to invalidate dependent resources
         */
        void InvalidateResourceWithReason(const std::string& name, InvalidationReason reason, bool propagateToDependents = true);

        /**
         * @brief Check if a specific binding point is invalidated
         * @param bindingPoint Binding point to check
         * @return true if any resource at that binding point is invalidated
         */
        bool IsBindingPointInvalidated(u32 bindingPoint) const;

        /**
         * @brief Get detailed invalidation information for a resource
         * @param name Resource name
         * @return Invalidation info or nullptr if not invalidated
         */
        const InvalidationInfo* GetInvalidationInfo(const std::string& name) const;

        /**
         * @brief Add a dependency relationship between two resources
         * @param dependentResource Resource that depends on the dependency
         * @param dependencyResource Resource that the dependent relies on
         */
        void AddResourceDependency(const std::string& dependentResource, const std::string& dependencyResource);

        /**
         * @brief Remove a dependency relationship
         * @param dependentResource Resource that depends on the dependency
         * @param dependencyResource Resource that the dependent relied on
         */
        void RemoveResourceDependency(const std::string& dependentResource, const std::string& dependencyResource);

        /**
         * @brief Get all resources that depend on the specified resource
         * @param name Resource name
         * @return Vector of dependent resource names
         */
        std::vector<std::string> GetResourceDependents(const std::string& name) const;

        /**
         * @brief Get all resources that the specified resource depends on
         * @param name Resource name
         * @return Vector of dependency resource names
         */
        std::vector<std::string> GetResourceDependencies(const std::string& name) const;

        // Phase 4.2: Batch Update Optimization
        /**
         * @brief Schedule updates for efficient batch processing
         * @param maxBatchSize Maximum number of resources per batch (0 = unlimited)
         * @param priorityThreshold Only process batches at or above this priority
         */
        void ScheduleBatchUpdates(u32 maxBatchSize = 0, UpdatePriority priorityThreshold = UpdatePriority::Low);

        /**
         * @brief Process all scheduled update batches
         * @param frameNumber Current frame number for scheduling
         * @return Number of batches processed
         */
        u32 ProcessUpdateBatches(u32 frameNumber);

        /**
         * @brief Set update priority for a resource type
         * @param resourceType Type of resource
         * @param priority Priority level for updates
         */
        void SetResourceTypePriority(ShaderResourceType resourceType, UpdatePriority priority);

        /**
         * @brief Set update priority for a specific resource
         * @param name Resource name
         * @param priority Priority level for updates
         */
        void SetResourcePriority(const std::string& name, UpdatePriority priority);

        /**
         * @brief Get current update statistics
         * @return Statistics about update operations
         */
        const UpdateStatistics& GetUpdateStatistics() const { return m_UpdateStats; }

        /**
         * @brief Reset update statistics
         */
        void ResetUpdateStatistics() { m_UpdateStats.Reset(); }

        /**
         * @brief Enable or disable frame-based batching
         * @param enabled Whether to enable frame-based batching
         * @param maxFrameDelay Maximum frames to defer low-priority updates
         */
        void EnableFrameBasedBatching(bool enabled, u32 maxFrameDelay = 3);

        /**
         * @brief Force immediate processing of all pending updates (bypasses batching)
         */
        void FlushAllUpdates();

        // ==========================================
        // Phase 5: Enhanced Debug and Validation System
        // ==========================================

        // Phase 5.1: Resource Declaration System
        /**
         * @brief Get comprehensive resource declaration with SPIR-V metadata
         * @param name Resource name
         * @return Resource declaration or nullptr if not found
         */
        const ResourceDeclaration* GetResourceDeclaration(const std::string& name) const;

        /**
         * @brief Get all resource declarations
         * @return Map of resource name to declaration
         */
        const std::unordered_map<std::string, ResourceDeclaration>& GetResourceDeclarations() const;

        /**
         * @brief Update usage statistics for a resource
         * @param name Resource name
         * @param wasRead Whether resource was read from
         * @param wasWritten Whether resource was written to
         * @param bytesTransferred Number of bytes transferred
         */
        void UpdateResourceUsageStatistics(const std::string& name, bool wasRead, bool wasWritten, u64 bytesTransferred = 0);

        /**
         * @brief Set usage pattern for a resource (for optimization hints)
         * @param name Resource name
         * @param pattern Usage pattern
         */
        void SetResourceUsagePattern(const std::string& name, ResourceUsagePattern pattern);

        /**
         * @brief Get resource usage statistics
         * @param name Resource name
         * @return Usage statistics or nullptr if not found
         */
        const ResourceDeclaration::UsageStatistics* GetResourceUsageStatistics(const std::string& name) const;

        /**
         * @brief Detect binding point conflicts across all resources
         * @return Vector of validation issues
         */
        std::vector<RegistryValidationIssue> DetectBindingConflicts() const;

        // Phase 5.2: Advanced Validation
        /**
         * @brief Perform comprehensive resource validation
         * @param enableLifecycleValidation Whether to validate resource lifecycles
         * @param enableSizeValidation Whether to validate resource sizes and alignment
         * @param enableConflictDetection Whether to detect binding conflicts
         * @return Vector of validation issues found
         */
        std::vector<RegistryValidationIssue> ValidateResources(bool enableLifecycleValidation = true, 
                                                              bool enableSizeValidation = true, 
                                                              bool enableConflictDetection = true) const;

        /**
         * @brief Validate resource lifecycle state
         * @param name Resource name
         * @return Validation issues related to lifecycle
         */
        std::vector<RegistryValidationIssue> ValidateResourceLifecycle(const std::string& name) const;

        /**
         * @brief Validate resource size and alignment requirements
         * @param name Resource name
         * @return Validation issues related to size/alignment
         */
        std::vector<RegistryValidationIssue> ValidateResourceSizeAlignment(const std::string& name) const;

        /**
         * @brief Get current lifecycle state of a resource
         * @param name Resource name
         * @return Lifecycle info or nullptr if not found
         */
        const ResourceLifecycleInfo* GetResourceLifecycleInfo(const std::string& name) const;

        /**
         * @brief Update resource lifecycle state
         * @param name Resource name
         * @param newState New lifecycle state
         * @param errorMessage Optional error message if state change failed
         */
        void UpdateResourceLifecycleState(const std::string& name, ResourceLifecycleState newState, 
                                         const std::string& errorMessage = "");

        /**
         * @brief Set validation severity filter (only report issues at or above this level)
         * @param minSeverity Minimum severity to report
         */
        void SetValidationSeverityFilter(RegistryValidationSeverity minSeverity);

        /**
         * @brief Get all validation issues since last clear
         * @param severityFilter Only return issues at or above this severity
         * @return Vector of validation issues
         */
        std::vector<RegistryValidationIssue> GetValidationIssues(RegistryValidationSeverity severityFilter = RegistryValidationSeverity::Info) const;

        /**
         * @brief Clear all stored validation issues
         */
        void ClearValidationIssues();

        /**
         * @brief Enable or disable real-time validation
         * @param enabled Whether to perform validation on each operation
         */
        void EnableRealtimeValidation(bool enabled);

        /**
         * @brief Generate detailed resource usage report
         * @return String containing formatted usage report
         */
        std::string GenerateUsageReport() const;

        // Phase 1.3: Enhanced Resource Compatibility System
        /**
         * @brief Check if a resource input is compatible with a binding
         * @param binding Shader resource binding information
         * @param input Resource input to validate
         * @return true if resource is compatible, false otherwise
         */
        bool IsCompatibleResource(const ShaderResourceBinding& binding, const ShaderResourceInput& input) const;

        /**
         * @brief Map ShaderResourceType to OpenGL resource type for validation
         * @param type Shader resource type
         * @return OpenGL resource type enum
         */
        GLenum MapToOpenGLResourceType(ShaderResourceType type) const;

    private:
        // Associated shader
        Ref<Shader> m_Shader = nullptr;

        // Resource binding information discovered through reflection
        std::unordered_map<std::string, ShaderResourceBinding> m_ResourceBindings;

        // Currently bound resources
        std::unordered_map<std::string, ShaderResource> m_BoundResources;

        // Phase 2.1/2.2: Specification and template support
        UniformBufferRegistrySpecification m_Specification;
        bool m_IsTemplate = false;                    // Whether this registry is a template
        bool m_IsClone = false;                       // Whether this registry is a clone
        std::string m_TemplateName;                   // Template name (if this is a template)
        std::string m_SourceTemplateName;             // Source template name (if this is a clone)
        std::unordered_map<std::string, std::string> m_CloneMapping; // Mapping for cloned resource names

        // Phase 3.1: Multi-set management
        std::unordered_map<u32, DescriptorSetInfo> m_DescriptorSets;
        std::unordered_map<DescriptorSetPriority, u32> m_PriorityToSetMap;
        std::vector<u32> m_SetBindingOrder;
        bool m_UseSetPriority = true;
        bool m_AutoAssignSets = true;
        u32 m_StartSet = 0;
        u32 m_EndSet = 4;
        
        // Phase 3.2: Default resource system
        std::unordered_map<std::string, ShaderResourceInfo> m_DefaultResources;
        std::unordered_map<std::string, UniformBufferRegistrySpecification> m_ResourceTemplates;
        bool m_DefaultResourcesInitialized = false;

        // Phase 1.2: Two-Phase Resource Updates - Pending resources awaiting commit
        std::unordered_map<std::string, ShaderResource> m_PendingResources;
        std::unordered_set<std::string> m_InvalidatedResources;

        // ==========================================
        // Phase 4: Advanced Invalidation System
        // ==========================================

        // Phase 4.1: Granular invalidation tracking
        std::unordered_map<std::string, InvalidationInfo> m_InvalidationDetails;
        std::unordered_map<u32, std::vector<std::string>> m_BindingPointInvalidations; // binding point -> invalidated resources
        std::unordered_map<std::string, std::vector<std::string>> m_ResourceDependencies; // resource -> dependencies
        std::unordered_map<std::string, std::vector<std::string>> m_ResourceDependents;   // resource -> dependents
        u32 m_CurrentFrame = 0;

        // Phase 4.2: Batch update optimization
        std::vector<UpdateBatch> m_UpdateBatches;
        std::unordered_map<ShaderResourceType, UpdatePriority> m_ResourceTypePriorities;
        std::unordered_map<std::string, UpdatePriority> m_ResourcePriorities;
        UpdateStatistics m_UpdateStats;
        bool m_FrameBasedBatchingEnabled = false;
        u32 m_MaxFrameDelay = 3;
        u32 m_LastBatchFrame = 0;

        // Phase 5: Enhanced Debug and Validation System private members
        std::unordered_map<std::string, ResourceDeclaration> m_ResourceDeclarations;
        std::unordered_map<std::string, ResourceLifecycleInfo> m_ResourceLifecycleInfo;
        std::vector<RegistryValidationIssue> m_ValidationIssues;
        RegistryValidationSeverity m_ValidationSeverityFilter = RegistryValidationSeverity::Info;
        bool m_RealtimeValidationEnabled = false;

        // Bindings that need to be applied (dirty tracking)
        std::unordered_set<std::string> m_DirtyBindings;

        // Binding point usage tracking for validation
        std::unordered_map<u32, std::string> m_BindingPointUsage;

        // Initialization state
        bool m_Initialized = false;

        // Frame-in-flight support (Phase 1.3)
        std::unique_ptr<FrameInFlightManager> m_FrameInFlightManager = nullptr;
        bool m_FrameInFlightEnabled = false;

        // ==========================================
        // Phase 6: Performance and Usability
        // ==========================================

        // Phase 6.1: Resource Handle Caching
        ResourceHandleCache* m_HandleCache = nullptr;
        bool m_HandleCachingEnabled = true;

        // OpenGL descriptor set integration
        std::unique_ptr<OpenGLDescriptorSetManager> m_DescriptorSetManager;
        std::unique_ptr<OpenGLMultiBind> m_MultiBind;
        std::unique_ptr<DSABindingManager> m_DSABindingManager;
        bool m_DSAEnabled = true;                    // Whether to use DSA when available

        // Binding state caching
        bool m_CacheEnabled = true;                  // Whether caching is enabled
        bool m_CacheInitialized = false;            // Whether cache has been initialized

        // OpenGL resource declarations for enhanced SPIR-V integration
        mutable std::unordered_map<std::string, std::unique_ptr<OpenGLResourceDeclaration>> m_OpenGLDeclarations;
        std::string m_DefaultPassName = "DefaultPass";

        // Phase 6.2: Enhanced Template Getter (no additional members needed, uses existing infrastructure)

        // Phase 2.1: Template and clone support methods
        
        /**
         * @brief Copy resource bindings from another registry (for templates/clones)
         * @param source Source registry to copy from
         * @param includeResources Whether to copy bound resources or just bindings
         */
        void CopyBindingsFrom(const UniformBufferRegistry& source, bool includeResources = false);

        /**
         * @brief Validate cloned registry against target shader
         * @param targetShader Shader to validate against
         * @return true if compatible, false otherwise
         */
        bool ValidateCloneCompatibility(const Ref<Shader>& targetShader) const;

        /**
         * @brief Apply specification settings to registry configuration
         */
        void ApplySpecificationSettings();

        /**
         * @brief Set up resource templates based on shader pattern detection
         */
        void SetupResourceTemplates();

        // Phase 3.1: Multi-set management private methods
        
        /**
         * @brief Initialize descriptor sets based on configuration
         */
        void InitializeDescriptorSets();

        /**
         * @brief Update set binding order based on priorities
         */
        void UpdateSetBindingOrder();

        /**
         * @brief Determine appropriate descriptor set for a resource based on heuristics
         * @param resourceName Name of the resource
         * @param resourceInfo Resource binding information
         * @return Suggested descriptor set priority
         */
        DescriptorSetPriority DetermineResourceSetPriority(const std::string& resourceName, 
                                                          const ShaderResourceBinding& resourceInfo) const;

        /**
         * @brief Validate descriptor set assignments
         * @return true if all assignments are valid
         */
        bool ValidateSetAssignments() const;

        // Phase 3.2: Default resource system private methods
        
        /**
         * @brief Initialize built-in resource templates
         */
        void InitializeBuiltinTemplates();

        /**
         * @brief Create default system uniform buffer (view/projection matrices, time, etc.)
         */
        void CreateDefaultSystemBuffer();

        /**
         * @brief Create default material uniform buffer based on shader uniforms
         */
        void CreateDefaultMaterialBuffer();

        /**
         * @brief Create default lighting uniform buffer
         */
        void CreateDefaultLightingBuffer();

        /**
         * @brief Analyze shader uniforms to detect common patterns
         * @return Detected shader pattern type
         */
        std::string AnalyzeShaderPattern() const;

        /**
         * @brief Set up default texture bindings based on shader samplers
         */
        void SetupDefaultTextures();

        // ==========================================
        // Phase 4: Advanced Invalidation System Private Methods
        // ==========================================

        // Phase 4.1: Granular invalidation tracking helpers
        /**
         * @brief Propagate invalidation to dependent resources
         * @param resourceName Name of the invalidated resource
         * @param reason Reason for the original invalidation
         */
        void PropagateInvalidationToDependents(const std::string& resourceName, InvalidationReason reason);

        /**
         * @brief Update binding point invalidation tracking
         * @param bindingPoint Binding point to update
         * @param resourceName Resource name that was invalidated
         * @param add Whether to add (true) or remove (false) the invalidation
         */
        void UpdateBindingPointInvalidation(u32 bindingPoint, const std::string& resourceName, bool add);

        /**
         * @brief Validate dependency graph for cycles
         * @return true if no cycles detected, false otherwise
         */
        bool ValidateDependencyGraph() const;

        // Phase 4.2: Batch update optimization helpers
        /**
         * @brief Create update batches from invalidated resources
         */
        void CreateUpdateBatches();

        /**
         * @brief Determine update priority for a resource
         * @param resourceName Name of the resource
         * @param resourceType Type of the resource
         * @return Calculated priority level
         */
        UpdatePriority DetermineUpdatePriority(const std::string& resourceName, ShaderResourceType resourceType) const;

        /**
         * @brief Calculate estimated cost for an update batch
         * @param batch Update batch to analyze
         * @return Estimated GPU state change cost
         */
        u32 CalculateBatchCost(const UpdateBatch& batch) const;

        /**
         * @brief Sort update batches by priority and cost
         */
        void SortUpdateBatches();

        /**
         * @brief Process a single update batch
         * @param batch Batch to process
         * @return true if batch was successfully processed
         */
        bool ProcessUpdateBatch(UpdateBatch& batch);

        /**
         * @brief Check if a batch should be processed in the current frame
         * @param batch Batch to check
         * @param currentFrame Current frame number
         * @return true if batch should be processed
         */
        bool ShouldProcessBatch(const UpdateBatch& batch, u32 currentFrame) const;

        // ==========================================
        // Phase 5: Enhanced Debug and Validation System Private Methods
        // ==========================================

        // Phase 5.1: Resource Declaration System helpers
        /**
         * @brief Initialize resource declaration from resource metadata
         * @param name Resource name
         * @param resourceType Resource type
         */
        void InitializeResourceDeclaration(const std::string& name, ShaderResourceType resourceType);

        /**
         * @brief Extract SPIR-V metadata for a resource (if available)
         * @param name Resource name
         * @return SPIR-V metadata or empty struct if not available
         */
        ResourceDeclaration::SPIRVMetadata ExtractSPIRVMetadata(const std::string& name) const;

        /**
         * @brief Validate resource usage pattern consistency
         * @param name Resource name
         * @param actualPattern Pattern observed in runtime
         * @param declaredPattern Pattern declared by user
         * @return Vector of validation issues
         */
        std::vector<RegistryValidationIssue> ValidateUsagePatternConsistency(const std::string& name, 
                                                                            ResourceUsagePattern actualPattern,
                                                                            ResourceUsagePattern declaredPattern) const;

        // Phase 5.2: Advanced Validation helpers
        /**
         * @brief Create validation issue with current context
         * @param severity Issue severity
         * @param category Issue category
         * @param message Issue message
         * @param resourceName Associated resource name (optional)
         * @return Created validation issue
         */
        RegistryValidationIssue CreateValidationIssue(RegistryValidationSeverity severity, 
                                                     const std::string& category,
                                                     const std::string& message,
                                                     const std::string& resourceName = "") const;

        /**
         * @brief Check if validation issue should be reported based on severity filter
         * @param issue Validation issue to check
         * @return True if issue should be reported
         */
        bool ShouldReportValidationIssue(const RegistryValidationIssue& issue) const;

        /**
         * @brief Perform lifecycle transition validation
         * @param name Resource name
         * @param fromState Current state
         * @param toState Target state
         * @return Vector of validation issues (empty if valid transition)
         */
        std::vector<RegistryValidationIssue> ValidateLifecycleTransition(const std::string& name,
                                                                        ResourceLifecycleState fromState,
                                                                        ResourceLifecycleState toState) const;

        // ==========================================
        // Step 12: Cache Helper Methods
        // ==========================================

        /**
         * @brief Get OpenGL target from shader resource type
         * @param type Shader resource type
         * @return Corresponding OpenGL target constant
         */
        GLenum GetOpenGLTargetFromType(ShaderResourceType type) const;
    };

    /**
     * @brief Phase 3 Advanced Usage Examples:
     * 
     * // Example 1: Modern PBR material with automatic set assignment
     * auto pbrSpec = UniformBufferRegistrySpecification{};
     * pbrSpec.Name = "PBRMaterial";
     * pbrSpec.Configuration = RegistryConfiguration::Performance;
     * pbrSpec.UseSetPriority = true;           // Enable multi-set management (DEFAULT)
     * pbrSpec.AutoAssignSets = true;           // Smart resource assignment (DEFAULT)
     * pbrSpec.EnableDefaultResources = true;   // Auto-populate common resources (DEFAULT)
     * pbrSpec.AutoDetectShaderPattern = true;  // Smart defaults based on shader (DEFAULT)
     * 
     * auto registry = CreateScope<UniformBufferRegistry>(pbrShader, pbrSpec);
     * registry->Initialize(); // Auto-creates SystemUniforms, MaterialUniforms, LightingUniforms
     * 
     * // Resources are automatically assigned to appropriate descriptor sets:
     * // Set 0 (System): ViewMatrix, ProjectionMatrix, CameraPosition, Time
     * // Set 1 (Global): LightingData, EnvironmentMap, ShadowMaps
     * // Set 2 (Material): DiffuseTexture, NormalTexture, MetallicRoughnessTexture
     * // Set 3 (Instance): ModelMatrix, InstanceData
     * 
     * // Binding is now optimized by set priority:
     * registry->BindAllSets(); // Binds in optimal order: System -> Global -> Material -> Instance
     * 
     * // Example 2: Integration with ResourceBindingGroup for batch operations
     * auto manager = CreateScope<ResourceBindingGroupManager>();
     * manager->SetRegistry(registry.get());
     * 
     * auto* materialGroup = manager->CreateGroup("PBRMaterialGroup");
     * materialGroup->SetBindingStrategy(BindingStrategy::Batched);
     * materialGroup->AddResource("DiffuseTexture", 0, diffuseTexture);
     * materialGroup->AddResource("NormalTexture", 1, normalTexture);
     * materialGroup->AddResource("MaterialProperties", 1, materialBuffer);
     * 
     * // Binding now uses Phase 3 multi-set optimization automatically:
     * materialGroup->Bind(); // Groups by descriptor set, binds in priority order
     * 
     * // Example 3: Template-based workflow for multiple similar materials
     * auto templateRegistry = UniformBufferRegistry::CreateTemplate(*registry, "PBRTemplate");
     * 
     * // Create instances for different materials using the template:
     * auto metalMaterial = UniformBufferRegistry::CreateFromTemplate(*templateRegistry, metalShader, "MetalMaterial");
     * auto fabricMaterial = UniformBufferRegistry::CreateFromTemplate(*templateRegistry, fabricShader, "FabricMaterial");
     * 
     * // All instances inherit the optimized set configuration and default resources
     * 
     * // Example 4: Advanced set-specific binding for performance
     * // Only bind system resources once per frame:
     * registry->BindDescriptorSet(0); // System set - view/projection matrices
     * 
     * // Bind global resources once per scene:
     * registry->BindDescriptorSet(1); // Global set - lighting, environment
     * 
     * // Bind material resources per material:
     * registry->BindDescriptorSet(2); // Material set - textures, material properties
     * 
     * // Bind instance resources per object:
     * registry->BindDescriptorSet(3); // Instance set - model matrix, instance data
     */

}  // End of OloEngine namespace
