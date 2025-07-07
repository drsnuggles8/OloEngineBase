#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ShaderResourceTypes.h"

#include <glad/gl.h>
#include <vector>
#include <array>

namespace OloEngine
{
    /**
     * @brief OpenGL 4.6 multi-bind utility for efficient batch resource binding
     * 
     * This class leverages OpenGL's multi-bind functions to reduce the number of
     * individual bind calls, improving performance for shaders with many resources.
     */
    class OpenGLMultiBind
    {
    public:
        /**
         * @brief Configuration for multi-bind operations
         */
        struct Config
        {
            bool EnableTextureBatching = true;    // Batch texture bindings
            bool EnableBufferBatching = true;     // Batch buffer bindings
            bool EnableValidation = true;         // Validate bindings before submission
            u32 MaxTexturesPerBatch = 32;         // Maximum textures per batch
            u32 MaxBuffersPerBatch = 32;          // Maximum buffers per batch
            bool UseDirectStateAccess = true;     // Use DSA where possible
            bool EnableCaching = true;            // Cache binding state
        };

        /**
         * @brief Batch information for texture bindings
         */
        struct TextureBatch
        {
            std::vector<u32> TextureIDs;          // OpenGL texture IDs
            std::vector<u32> BindingPoints;       // Binding points
            std::vector<GLenum> Targets;          // Texture targets (GL_TEXTURE_2D, etc.)
            ShaderResourceType ResourceType = ShaderResourceType::Texture2D;
            u32 StartBinding = 0;                 // Starting binding point
            u32 Count = 0;                        // Number of textures
            
            void Clear() { TextureIDs.clear(); BindingPoints.clear(); Targets.clear(); Count = 0; }
            bool IsEmpty() const { return Count == 0; }
            bool IsFull(u32 maxSize) const { return Count >= maxSize; }
        };

        /**
         * @brief Batch information for buffer bindings
         */
        struct BufferBatch
        {
            std::vector<u32> BufferIDs;           // OpenGL buffer IDs
            std::vector<u32> BindingPoints;       // Binding points
            std::vector<size_t> Offsets;          // Buffer offsets
            std::vector<size_t> Sizes;            // Buffer sizes
            GLenum Target = GL_UNIFORM_BUFFER;    // Buffer target
            ShaderResourceType ResourceType = ShaderResourceType::UniformBuffer;
            u32 StartBinding = 0;                 // Starting binding point
            u32 Count = 0;                        // Number of buffers
            
            void Clear() { BufferIDs.clear(); BindingPoints.clear(); Offsets.clear(); Sizes.clear(); Count = 0; }
            bool IsEmpty() const { return Count == 0; }
            bool IsFull(u32 maxSize) const { return Count >= maxSize; }
        };

        /**
         * @brief Binding state cache for avoiding redundant bindings
         */
        struct BindingStateCache
        {
            std::array<u32, 32> BoundTextures = {0};     // Currently bound textures
            std::array<u32, 32> BoundBuffers = {0};      // Currently bound buffers
            std::array<GLenum, 32> TextureTargets = {0}; // Texture targets
            std::array<GLenum, 32> BufferTargets = {0};  // Buffer targets
            u32 LastBoundTextureUnit = 0;                // Last active texture unit
            bool IsValid = false;                         // Whether cache is valid
            
            void Invalidate() { IsValid = false; std::fill(BoundTextures.begin(), BoundTextures.end(), 0); 
                              std::fill(BoundBuffers.begin(), BoundBuffers.end(), 0); }
            bool IsTextureBound(u32 binding, u32 textureID, GLenum target) const
            {
                return IsValid && binding < BoundTextures.size() && 
                       BoundTextures[binding] == textureID && TextureTargets[binding] == target;
            }
            bool IsBufferBound(u32 binding, u32 bufferID, GLenum target) const
            {
                return IsValid && binding < BoundBuffers.size() && 
                       BoundBuffers[binding] == bufferID && BufferTargets[binding] == target;
            }
        };

    public:
        OpenGLMultiBind();
        explicit OpenGLMultiBind(const Config& config);
        ~OpenGLMultiBind() = default;

        // Configuration
        void SetConfig(const Config& config) { m_Config = config; }
        const Config& GetConfig() const { return m_Config; }

        // Texture binding operations
        /**
         * @brief Add a texture to the current batch
         * @param textureID OpenGL texture ID
         * @param bindingPoint Binding point for the texture
         * @param target Texture target (GL_TEXTURE_2D, GL_TEXTURE_CUBE_MAP, etc.)
         * @param resourceType Resource type for validation
         */
        void AddTexture(u32 textureID, u32 bindingPoint, GLenum target, ShaderResourceType resourceType);

        /**
         * @brief Add multiple textures to the current batch
         * @param textureIDs Array of texture IDs
         * @param bindingPoints Array of binding points
         * @param targets Array of texture targets
         * @param count Number of textures
         * @param resourceType Resource type for validation
         */
        void AddTextures(const u32* textureIDs, const u32* bindingPoints, const GLenum* targets, 
                        u32 count, ShaderResourceType resourceType);

        /**
         * @brief Submit all pending texture bindings
         */
        void SubmitTextures();

        // Buffer binding operations
        /**
         * @brief Add a buffer to the current batch
         * @param bufferID OpenGL buffer ID
         * @param bindingPoint Binding point for the buffer
         * @param target Buffer target (GL_UNIFORM_BUFFER, GL_SHADER_STORAGE_BUFFER)
         * @param offset Buffer offset (0 for full buffer)
         * @param size Buffer size (0 for full buffer)
         * @param resourceType Resource type for validation
         */
        void AddBuffer(u32 bufferID, u32 bindingPoint, GLenum target, size_t offset, size_t size, 
                      ShaderResourceType resourceType);

        /**
         * @brief Add multiple buffers to the current batch
         * @param bufferIDs Array of buffer IDs
         * @param bindingPoints Array of binding points
         * @param offsets Array of buffer offsets
         * @param sizes Array of buffer sizes
         * @param count Number of buffers
         * @param target Buffer target
         * @param resourceType Resource type for validation
         */
        void AddBuffers(const u32* bufferIDs, const u32* bindingPoints, const size_t* offsets, 
                       const size_t* sizes, u32 count, GLenum target, ShaderResourceType resourceType);

        /**
         * @brief Submit all pending buffer bindings
         */
        void SubmitBuffers();

        // Batch operations
        /**
         * @brief Submit all pending bindings (textures and buffers)
         */
        void SubmitAll();

        /**
         * @brief Clear all pending bindings without submitting
         */
        void Clear();

        /**
         * @brief Check if there are pending bindings
         */
        bool HasPendingBindings() const;

        // State management
        /**
         * @brief Invalidate binding state cache
         */
        void InvalidateCache() { m_StateCache.Invalidate(); }

        /**
         * @brief Enable or disable state caching
         */
        void SetCachingEnabled(bool enabled) { m_Config.EnableCaching = enabled; if (!enabled) InvalidateCache(); }

        // Statistics and debugging
        struct Statistics
        {
            u32 TotalTextureBatches = 0;      // Total texture batches submitted
            u32 TotalBufferBatches = 0;       // Total buffer batches submitted
            u32 TotalTextureBindings = 0;     // Total individual texture bindings
            u32 TotalBufferBindings = 0;      // Total individual buffer bindings
            u32 CacheHits = 0;                // Number of cache hits
            u32 CacheMisses = 0;              // Number of cache misses
            f32 AverageBatchSize = 0.0f;      // Average batch size
            u32 RedundantBindingsPrevented = 0; // Redundant bindings prevented by caching

            void Reset() { *this = Statistics{}; }
            f32 GetCacheHitRatio() const { return (CacheHits + CacheMisses) > 0 ? (f32)CacheHits / (CacheHits + CacheMisses) : 0.0f; }
        };

        const Statistics& GetStatistics() const { return m_Statistics; }
        void ResetStatistics() { m_Statistics.Reset(); }

        // OpenGL capability checking
        /**
         * @brief Check if multi-bind is supported by the current OpenGL context
         */
        static bool IsMultiBindSupported();

        /**
         * @brief Check if Direct State Access is supported
         */
        static bool IsDSASupported();

        /**
         * @brief Get maximum number of texture units
         */
        static u32 GetMaxTextureUnits();

        /**
         * @brief Get maximum number of uniform buffer bindings
         */
        static u32 GetMaxUniformBufferBindings();

        /**
         * @brief Get maximum number of shader storage buffer bindings
         */
        static u32 GetMaxShaderStorageBufferBindings();

    private:
        // Internal binding implementation
        void SubmitTextureBatch(const TextureBatch& batch);
        void SubmitBufferBatch(const BufferBatch& batch);

        // Validation helpers
        bool ValidateTextureBatch(const TextureBatch& batch) const;
        bool ValidateBufferBatch(const BufferBatch& batch) const;

        // Cache management
        void UpdateTextureCache(u32 binding, u32 textureID, GLenum target);
        void UpdateBufferCache(u32 binding, u32 bufferID, GLenum target);

        // Batch management
        void FlushTextureBatchIfNeeded();
        void FlushBufferBatchIfNeeded();

    private:
        Config m_Config;
        
        // Current batches
        TextureBatch m_CurrentTextureBatch;
        BufferBatch m_CurrentBufferBatch;
        
        // State caching
        BindingStateCache m_StateCache;
        
        // Statistics
        mutable Statistics m_Statistics;
        
        // OpenGL capabilities (cached)
        static bool s_CapabilitiesQueried;
        static bool s_MultiBindSupported;
        static bool s_DSASupported;
        static u32 s_MaxTextureUnits;
        static u32 s_MaxUniformBufferBindings;
        static u32 s_MaxShaderStorageBufferBindings;
        
        // Initialize OpenGL capabilities
        static void QueryCapabilities();
    };
}
