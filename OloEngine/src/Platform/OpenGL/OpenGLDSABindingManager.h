#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RendererTypes.h"
#include <unordered_map>
#include <vector>
#include <glad/gl.h>

namespace OloEngine
{
    // Forward declarations
    class UniformBufferRegistry;
    struct ShaderResourceBinding;

    /**
     * @brief DSA binding state information for tracking and optimization
     */
    struct DSABindingState
    {
        u32 BufferHandle = 0;           // OpenGL buffer/texture handle
        u32 BindingPoint = 0;           // Binding point index
        u32 LastBoundFrame = 0;         // Frame when last bound
        bool IsDirty = true;            // Needs rebinding
        bool IsActive = false;          // Currently bound
        GLenum Target = 0;              // OpenGL target (GL_UNIFORM_BUFFER, etc.)
        size_t Size = 0;                // Buffer size in bytes
        GLintptr Offset = 0;            // Offset for range bindings
        GLsizeiptr Range = 0;           // Range size for range bindings (0 = full buffer)
        
        DSABindingState() = default;
        DSABindingState(u32 handle, u32 binding, GLenum target, size_t size = 0)
            : BufferHandle(handle), BindingPoint(binding), Target(target), Size(size) {}
    };

    /**
     * @brief Statistics for DSA binding operations
     */
    struct DSAStatistics
    {
        u32 TotalBindings = 0;          // Total binding operations performed
        u32 SkippedRedundant = 0;       // Redundant bindings skipped
        u32 StateChanges = 0;           // Actual OpenGL state changes
        u32 BatchedBindings = 0;        // Bindings performed in batches
        u32 RangeBindings = 0;          // Range-based bindings used
        f32 AverageBindTime = 0.0f;     // Average time per bind operation (ms)
        f32 TotalBindTime = 0.0f;       // Total time spent binding (ms)
        f32 EfficiencyRatio = 0.0f;     // Ratio of useful bindings to total attempts
        
        void Reset()
        {
            TotalBindings = SkippedRedundant = StateChanges = BatchedBindings = RangeBindings = 0;
            AverageBindTime = TotalBindTime = EfficiencyRatio = 0.0f;
        }
        
        void UpdateEfficiency()
        {
            if (TotalBindings > 0)
                EfficiencyRatio = static_cast<f32>(StateChanges) / static_cast<f32>(TotalBindings);
        }
    };

    /**
     * @brief Direct State Access binding manager for efficient OpenGL 4.5+ resource binding
     * 
     * This class leverages OpenGL 4.5+ Direct State Access features to provide efficient
     * resource binding with state tracking, redundancy elimination, and batch operations.
     */
    class DSABindingManager
    {
    public:
        /**
         * @brief DSA feature support flags
         */
        enum class DSAFeature : u32
        {
            None = 0,
            NamedBufferStorage = 1 << 0,        // glNamedBufferStorage
            NamedBufferSubData = 1 << 1,        // glNamedBufferSubData
            BindBufferRange = 1 << 2,           // glBindBufferRange
            BindBuffersRange = 1 << 3,          // glBindBuffersRange (4.4+)
            MultiBindBuffers = 1 << 4,          // glBindBuffersBase (4.4+)
            TextureStorage = 1 << 5,            // glTextureStorage2D, etc.
            TextureSubImage = 1 << 6,           // glTextureSubImage2D, etc.
            BindTextureUnit = 1 << 7,           // glBindTextureUnit (4.5+)
            BindImageTextures = 1 << 8,         // glBindImageTextures (4.4+)
            ProgramUniform = 1 << 9,            // glProgramUniform* (4.1+)
            InvalidateBufferData = 1 << 10,     // glInvalidateBufferData (4.3+)
            All = 0xFFFFFFFF
        };

        DSABindingManager();
        ~DSABindingManager();

        // Disable copy semantics to prevent accidental copying of binding state
        DSABindingManager(const DSABindingManager&) = delete;
        DSABindingManager& operator=(const DSABindingManager&) = delete;

        /**
         * @brief Initialize DSA binding manager and detect available features
         * @return True if DSA features are available, false otherwise
         */
        bool Initialize();

        /**
         * @brief Shutdown the DSA binding manager
         */
        void Shutdown();

        /**
         * @brief Check if specific DSA features are supported
         * @param features Features to check (can be OR'd together)
         * @return True if all specified features are supported
         */
        bool IsFeatureSupported(DSAFeature features) const;

        /**
         * @brief Get supported DSA feature flags
         * @return Bitmask of supported DSA features
         */
        u32 GetSupportedFeatures() const { return m_SupportedFeatures; }

        /**
         * @brief Bind a uniform buffer using DSA
         * @param bindingPoint Uniform buffer binding point
         * @param bufferHandle OpenGL buffer handle
         * @param offset Offset into buffer (0 for full buffer)
         * @param size Size to bind (0 for full buffer)
         * @return True if binding was successful
         */
        bool BindUniformBuffer(u32 bindingPoint, u32 bufferHandle, GLintptr offset = 0, GLsizeiptr size = 0);

        /**
         * @brief Bind a storage buffer using DSA
         * @param bindingPoint Storage buffer binding point
         * @param bufferHandle OpenGL buffer handle
         * @param offset Offset into buffer (0 for full buffer)
         * @param size Size to bind (0 for full buffer)
         * @return True if binding was successful
         */
        bool BindStorageBuffer(u32 bindingPoint, u32 bufferHandle, GLintptr offset = 0, GLsizeiptr size = 0);

        /**
         * @brief Bind a texture using DSA
         * @param textureUnit Texture unit index
         * @param textureHandle OpenGL texture handle
         * @return True if binding was successful
         */
        bool BindTexture(u32 textureUnit, u32 textureHandle);

        /**
         * @brief Bind multiple uniform buffers in a single call (if supported)
         * @param firstBinding First binding point
         * @param bindings Vector of binding information
         * @return Number of buffers successfully bound
         */
        u32 BindUniformBuffers(u32 firstBinding, const std::vector<DSABindingState>& bindings);

        /**
         * @brief Bind multiple storage buffers in a single call (if supported)
         * @param firstBinding First binding point
         * @param bindings Vector of binding information
         * @return Number of buffers successfully bound
         */
        u32 BindStorageBuffers(u32 firstBinding, const std::vector<DSABindingState>& bindings);

        /**
         * @brief Bind multiple textures in a single call (if supported)
         * @param firstUnit First texture unit
         * @param textureHandles Vector of texture handles
         * @return Number of textures successfully bound
         */
        u32 BindTextures(u32 firstUnit, const std::vector<u32>& textureHandles);

        /**
         * @brief Apply all resource bindings from a registry using DSA
         * @param registry Registry containing resource bindings
         * @param enableBatching Whether to use batch operations when possible
         * @return Number of resources successfully bound
         */
        u32 ApplyRegistryBindings(const UniformBufferRegistry& registry, bool enableBatching = true);

        /**
         * @brief Unbind resource at specific binding point
         * @param bindingPoint Binding point to unbind
         * @param target OpenGL target (GL_UNIFORM_BUFFER, GL_SHADER_STORAGE_BUFFER, etc.)
         */
        void UnbindResource(u32 bindingPoint, GLenum target);

        /**
         * @brief Unbind all resources of a specific type
         * @param target OpenGL target to unbind
         */
        void UnbindAllResources(GLenum target);

        /**
         * @brief Clear all binding state (does not unbind from OpenGL)
         */
        void ClearBindingState();

        /**
         * @brief Force rebind all resources on next application
         */
        void InvalidateAllBindings();

        /**
         * @brief Mark a specific binding as dirty
         * @param bindingPoint Binding point to invalidate
         * @param target OpenGL target
         */
        void InvalidateBinding(u32 bindingPoint, GLenum target);

        /**
         * @brief Check if a resource is currently bound
         * @param bindingPoint Binding point to check
         * @param target OpenGL target
         * @return True if resource is bound at the specified point
         */
        bool IsResourceBound(u32 bindingPoint, GLenum target) const;

        /**
         * @brief Get current binding state for a specific point
         * @param bindingPoint Binding point to query
         * @param target OpenGL target
         * @return Pointer to binding state, or nullptr if not bound
         */
        const DSABindingState* GetBindingState(u32 bindingPoint, GLenum target) const;

        /**
         * @brief Update frame counter for binding tracking
         * @param frameNumber Current frame number
         */
        void SetCurrentFrame(u32 frameNumber) { m_CurrentFrame = frameNumber; }

        /**
         * @brief Get DSA binding statistics
         * @return Current statistics
         */
        const DSAStatistics& GetStatistics() const { return m_Statistics; }

        /**
         * @brief Reset DSA binding statistics
         */
        void ResetStatistics() { m_Statistics.Reset(); }

        /**
         * @brief Enable or disable redundancy checking
         * @param enabled Whether to check for redundant bindings
         */
        void SetRedundancyCheckingEnabled(bool enabled) { m_RedundancyCheckingEnabled = enabled; }

        /**
         * @brief Enable or disable batched operations
         * @param enabled Whether to use batch operations when possible
         */
        void SetBatchingEnabled(bool enabled) { m_BatchingEnabled = enabled; }

        /**
         * @brief Set maximum number of resources to batch in a single call
         * @param maxBatch Maximum batch size (0 = unlimited)
         */
        void SetMaxBatchSize(u32 maxBatch) { m_MaxBatchSize = maxBatch; }

        /**
         * @brief Get OpenGL version information
         */
        struct OpenGLVersionInfo
        {
            int Major = 0;
            int Minor = 0;
            bool HasDSA = false;            // OpenGL 4.5+
            bool HasMultiBind = false;      // OpenGL 4.4+
            bool HasBufferStorage = false;  // OpenGL 4.4+
            std::string Renderer;
            std::string Version;
        };

        const OpenGLVersionInfo& GetVersionInfo() const { return m_VersionInfo; }

    private:
        // DSA capability flags
        u32 m_SupportedFeatures = 0;
        bool m_IsInitialized = false;
        bool m_RedundancyCheckingEnabled = true;
        bool m_BatchingEnabled = true;
        u32 m_MaxBatchSize = 32;           // Maximum resources to batch
        u32 m_CurrentFrame = 0;

        // OpenGL version and capability information
        OpenGLVersionInfo m_VersionInfo;

        // Binding state tracking
        std::unordered_map<u64, DSABindingState> m_BindingStates; // Key: (target << 32) | bindingPoint
        std::vector<DSABindingState> m_BatchBuffer;               // Temporary buffer for batch operations
        
        // Performance statistics
        mutable DSAStatistics m_Statistics;

        /**
         * @brief Generate unique key for binding state storage
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @return Unique 64-bit key
         */
        u64 GenerateBindingKey(GLenum target, u32 bindingPoint) const;

        /**
         * @brief Detect available DSA features
         */
        void DetectDSAFeatures();

        /**
         * @brief Check if binding is redundant
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @param bufferHandle Buffer handle to check
         * @param offset Buffer offset
         * @param size Buffer size
         * @return True if binding would be redundant
         */
        bool IsBindingRedundant(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                              GLintptr offset, GLsizeiptr size) const;

        /**
         * @brief Update binding state after successful bind
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @param bufferHandle Buffer handle
         * @param offset Buffer offset
         * @param size Buffer size
         */
        void UpdateBindingState(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                              GLintptr offset, GLsizeiptr size);

        /**
         * @brief Update statistics for a binding operation
         * @param wasRedundant Whether the binding was redundant
         * @param wasBatched Whether the binding was part of a batch
         * @param bindTime Time taken for the bind operation
         */
        void UpdateStatistics(bool wasRedundant, bool wasBatched, f32 bindTime) const;

        /**
         * @brief Perform actual OpenGL binding call
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @param bufferHandle Buffer handle
         * @param offset Buffer offset
         * @param size Buffer size
         * @return True if successful
         */
        bool PerformBinding(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                          GLintptr offset, GLsizeiptr size);

        /**
         * @brief Perform batch binding operation
         * @param target OpenGL target
         * @param firstBinding First binding point
         * @param bindings Vector of binding states
         * @return Number of successful bindings
         */
        u32 PerformBatchBinding(GLenum target, u32 firstBinding, const std::vector<DSABindingState>& bindings);

        /**
         * @brief Get buffer size from OpenGL handle (for validation)
         * @param bufferHandle Buffer handle
         * @return Buffer size in bytes, or 0 if unable to determine
         */
        size_t GetBufferSize(u32 bufferHandle) const;

        /**
         * @brief Validate binding parameters
         * @param target OpenGL target
         * @param bindingPoint Binding point
         * @param bufferHandle Buffer handle
         * @param offset Buffer offset
         * @param size Buffer size
         * @return True if parameters are valid
         */
        bool ValidateBindingParameters(GLenum target, u32 bindingPoint, u32 bufferHandle, 
                                     GLintptr offset, GLsizeiptr size) const;
    };

    // Utility functions for DSA feature detection
    
    /**
     * @brief Check if OpenGL 4.5+ DSA is available
     * @return True if DSA is supported
     */
    bool IsDSASupported();

    /**
     * @brief Get human-readable name for DSA feature
     * @param feature DSA feature flag
     * @return Feature name string
     */
    const char* GetDSAFeatureName(DSABindingManager::DSAFeature feature);

    /**
     * @brief Enable DSA feature flags using bitwise OR
     */
    constexpr DSABindingManager::DSAFeature operator|(DSABindingManager::DSAFeature a, DSABindingManager::DSAFeature b)
    {
        return static_cast<DSABindingManager::DSAFeature>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    constexpr DSABindingManager::DSAFeature operator&(DSABindingManager::DSAFeature a, DSABindingManager::DSAFeature b)
    {
        return static_cast<DSABindingManager::DSAFeature>(static_cast<u32>(a) & static_cast<u32>(b));
    }
}
