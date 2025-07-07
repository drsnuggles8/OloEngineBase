#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ShaderResourceTypes.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <glad/gl.h>

namespace OloEngine
{
    /**
     * @brief OpenGL adaptation of Hazel's ShaderResourceDeclaration
     * 
     * This class provides a comprehensive declaration system for OpenGL resources
     * that mimics Hazel's descriptor set approach but adapted for OpenGL binding points.
     */
    class OpenGLResourceDeclaration
    {
    public:
        /**
         * @brief Resource access pattern for optimization hints
         */
        enum class AccessPattern : u8
        {
            ReadOnly = 0,      // Resource is only read (textures, constants)
            WriteOnly,         // Resource is only written (render targets)
            ReadWrite,         // Resource is both read and written (SSBOs)
            Static,            // Resource data never changes
            Dynamic,           // Resource data changes frequently
            Streaming          // Resource data is updated every frame
        };

        /**
         * @brief Resource usage frequency for caching optimization
         */
        enum class UsageFrequency : u8
        {
            Never = 0,         // Resource is declared but not used
            Rare,              // Used occasionally
            Normal,            // Standard usage
            Frequent,          // Used every few frames
            Constant           // Used every frame
        };

        /**
         * @brief Detailed resource declaration information
         */
        struct ResourceInfo
        {
            std::string Name;                           // Resource name in shader
            ShaderResourceType Type = ShaderResourceType::None;
            u32 Set = 0;                                // Descriptor set index (Hazel compatibility)
            u32 Binding = UINT32_MAX;                   // OpenGL binding point
            u32 Location = UINT32_MAX;                  // GLSL location (for vertex attributes)
            u32 Size = 0;                               // Size in bytes (for buffers)
            u32 ArraySize = 1;                          // Array size (1 for non-arrays)
            bool IsArray = false;                       // Whether this is an array resource
            bool IsOptional = false;                    // Whether resource binding is optional
            AccessPattern Access = AccessPattern::ReadOnly;
            UsageFrequency Frequency = UsageFrequency::Normal;
            
            // OpenGL-specific information
            GLenum GLType = 0;                          // OpenGL type (GL_FLOAT, GL_INT, etc.)
            GLenum GLTarget = 0;                        // OpenGL target (GL_TEXTURE_2D, etc.)
            GLenum GLFormat = 0;                        // OpenGL format (GL_RGBA8, etc.)
            GLenum GLInternalFormat = 0;                // OpenGL internal format
            u32 GLComponents = 0;                       // Number of components (1-4)
            bool GLNormalized = false;                  // Whether values are normalized
            
            // SPIR-V reflection metadata
            u32 SPIRVTypeID = 0;                        // SPIR-V type ID
            u32 SPIRVBaseTypeID = 0;                    // Base type for structs/arrays
            std::vector<u32> SPIRVMemberOffsets;       // Member offsets for structs
            std::vector<std::string> SPIRVMemberNames; // Member names for structs
            std::vector<u32> SPIRVMemberTypes;         // Member type IDs for structs
            
            // Usage statistics and optimization hints
            u32 EstimatedUpdateFrequency = 0;          // Updates per frame estimate
            u32 EstimatedMemoryUsage = 0;              // Memory usage estimate in bytes
            f32 Priority = 1.0f;                       // Binding priority (higher = more important)
            
            ResourceInfo() = default;
            ResourceInfo(const std::string& name, ShaderResourceType type, u32 binding)
                : Name(name), Type(type), Binding(binding) {}
        };

        /**
         * @brief Resource declaration similar to Hazel's RenderPassInputDeclaration
         */
        struct InputDeclaration
        {
            std::string PassName;                       // Name of the pass/shader
            std::vector<ResourceInfo> Resources;        // All declared resources
            std::unordered_map<std::string, u32> NameToIndex; // Fast name lookup
            std::unordered_map<u32, std::vector<u32>> SetToResources; // Set-based grouping
            
            // Validation and optimization metadata
            u32 TotalUniformBuffers = 0;
            u32 TotalStorageBuffers = 0;
            u32 TotalTextures = 0;
            u32 TotalImages = 0;
            u32 TotalMemoryUsage = 0;                   // Estimated total memory usage
            bool IsValid = false;                       // Whether declaration is valid
            std::vector<std::string> ValidationErrors; // Validation error messages
            
            InputDeclaration() = default;
            InputDeclaration(const std::string& passName) : PassName(passName) {}
        };

    public:
        OpenGLResourceDeclaration() = default;
        explicit OpenGLResourceDeclaration(const std::string& passName);
        ~OpenGLResourceDeclaration() = default;

        // Resource declaration management
        /**
         * @brief Add a resource declaration
         * @param resourceInfo Resource information to add
         * @return Index of added resource or UINT32_MAX if failed
         */
        u32 AddResource(const ResourceInfo& resourceInfo);

        /**
         * @brief Remove a resource declaration
         * @param name Name of resource to remove
         * @return true if resource was removed
         */
        bool RemoveResource(const std::string& name);

        /**
         * @brief Get resource information by name
         * @param name Name of resource
         * @return Pointer to resource info or nullptr if not found
         */
        const ResourceInfo* GetResource(const std::string& name) const;

        /**
         * @brief Get resource information by index
         * @param index Index of resource
         * @return Pointer to resource info or nullptr if invalid index
         */
        const ResourceInfo* GetResource(u32 index) const;

        /**
         * @brief Update resource information
         * @param name Name of resource to update
         * @param resourceInfo New resource information
         * @return true if resource was updated
         */
        bool UpdateResource(const std::string& name, const ResourceInfo& resourceInfo);

        // SPIR-V integration
        /**
         * @brief Populate declarations from SPIR-V reflection data
         * @param stage Shader stage (GL_VERTEX_SHADER, etc.)
         * @param spirvData SPIR-V bytecode
         * @return true if reflection was successful
         */
        bool PopulateFromSPIRV(u32 stage, const std::vector<u32>& spirvData);

        /**
         * @brief Extract resource information from SPIR-V compiler
         * @param compiler SPIR-V cross compiler instance
         * @param stage Shader stage for context
         */
        void ExtractFromSPIRVCompiler(const spirv_cross::Compiler& compiler, u32 stage);

        // Hazel compatibility
        /**
         * @brief Convert from Hazel's ShaderResourceDeclaration
         * @param hazelDeclaration Hazel resource declaration
         * @param set Descriptor set index
         * @param globalBindingOffset Global binding offset for this set
         */
        void ImportFromHazel(const ShaderResourceDeclaration& hazelDeclaration, u32 set, u32 globalBindingOffset = 0);

        /**
         * @brief Export to Hazel-compatible format
         * @param set Descriptor set to export
         * @return Vector of Hazel-compatible declarations
         */
        std::vector<ShaderResourceDeclaration> ExportToHazel(u32 set) const;

        // Validation and optimization
        /**
         * @brief Validate all resource declarations
         * @return true if all declarations are valid
         */
        bool Validate();

        /**
         * @brief Check for binding conflicts
         * @return Vector of conflicting resource names
         */
        std::vector<std::string> FindBindingConflicts() const;

        /**
         * @brief Optimize binding layout for performance
         * @param enableAutomaticReordering Whether to automatically reorder bindings
         * @return true if optimization was successful
         */
        bool OptimizeBindingLayout(bool enableAutomaticReordering = true);

        /**
         * @brief Generate binding ranges for descriptor sets
         * @param maxSets Maximum number of descriptor sets to use
         * @return Map of set index to binding ranges
         */
        std::unordered_map<u32, std::pair<u32, u32>> GenerateSetBindingRanges(u32 maxSets = 4) const;

        // Access and querying
        /**
         * @brief Get all resources of a specific type
         * @param type Resource type to filter by
         * @return Vector of resource indices
         */
        std::vector<u32> GetResourcesByType(ShaderResourceType type) const;

        /**
         * @brief Get all resources in a specific descriptor set
         * @param set Descriptor set index
         * @return Vector of resource indices
         */
        std::vector<u32> GetResourcesBySet(u32 set) const;

        /**
         * @brief Get resources with specific access pattern
         * @param pattern Access pattern to filter by
         * @return Vector of resource indices
         */
        std::vector<u32> GetResourcesByAccessPattern(AccessPattern pattern) const;

        /**
         * @brief Check if a resource exists
         * @param name Resource name to check
         * @return true if resource exists
         */
        bool HasResource(const std::string& name) const;

        /**
         * @brief Get total number of declared resources
         * @return Number of resources
         */
        u32 GetResourceCount() const { return static_cast<u32>(m_Declaration.Resources.size()); }

        /**
         * @brief Get the input declaration
         * @return Reference to input declaration
         */
        const InputDeclaration& GetDeclaration() const { return m_Declaration; }

        // Statistics and debugging
        /**
         * @brief Generate resource usage report
         * @return Formatted string with resource statistics
         */
        std::string GenerateUsageReport() const;

        /**
         * @brief Export declarations to JSON format
         * @param includeMetadata Whether to include SPIR-V metadata
         * @return JSON string representation
         */
        std::string ExportToJSON(bool includeMetadata = false) const;

        /**
         * @brief Import declarations from JSON format
         * @param jsonData JSON string data
         * @return true if import was successful
         */
        bool ImportFromJSON(const std::string& jsonData);

        /**
         * @brief Render ImGui debug interface
         */
        void RenderDebugInterface();

        // Utility functions for OpenGL types
        /**
         * @brief Convert SPIR-V type to OpenGL type
         * @param spirvType SPIR-V type ID
         * @param compiler SPIR-V compiler for type lookup
         * @return OpenGL type enum
         */
        static GLenum SPIRVToGLType(u32 spirvType, const spirv_cross::Compiler& compiler);

        /**
         * @brief Convert resource type to OpenGL target
         * @param resourceType Shader resource type
         * @return OpenGL target enum
         */
        static GLenum ResourceTypeToGLTarget(ShaderResourceType resourceType);

        /**
         * @brief Estimate memory usage for a resource
         * @param resourceInfo Resource information
         * @return Estimated memory usage in bytes
         */
        static u32 EstimateMemoryUsage(const ResourceInfo& resourceInfo);

    private:
        // Internal helpers
        void UpdateIndices();
        void UpdateStatistics();
        bool ValidateResource(const ResourceInfo& resource, u32 index) const;
        void AssignAutomaticBindings();
        void GroupResourcesBySets();

        // SPIR-V reflection helpers
        void ProcessUniformBuffers(const spirv_cross::Compiler& compiler, const spirv_cross::ShaderResources& resources);
        void ProcessStorageBuffers(const spirv_cross::Compiler& compiler, const spirv_cross::ShaderResources& resources);
        void ProcessTextures(const spirv_cross::Compiler& compiler, const spirv_cross::ShaderResources& resources);
        void ProcessImages(const spirv_cross::Compiler& compiler, const spirv_cross::ShaderResources& resources);
        void ProcessPushConstants(const spirv_cross::Compiler& compiler, const spirv_cross::ShaderResources& resources);

        ResourceInfo CreateResourceFromSPIRV(const spirv_cross::Compiler& compiler, 
                                            const spirv_cross::Resource& resource, 
                                            ShaderResourceType type) const;

    private:
        InputDeclaration m_Declaration;
        bool m_AutoAssignBindings = true;           // Whether to automatically assign binding points
        bool m_EnableOptimization = true;          // Whether to enable automatic optimization
        u32 m_NextAutoBinding = 0;                 // Next automatic binding point
        std::unordered_map<ShaderResourceType, u32> m_TypeBindingCounters; // Per-type binding counters
    };
}
