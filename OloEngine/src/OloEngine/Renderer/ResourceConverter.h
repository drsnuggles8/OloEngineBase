#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RendererTypes.h"
#include "Platform/OpenGL/OpenGLResourceDeclaration.h"
#include <functional>
#include <unordered_map>
#include <typeindex>

namespace OloEngine
{
    // Forward declarations
    class UniformBufferRegistry;

    /**
     * @brief Conversion compatibility information between shader resource types
     */
    struct ConversionCompatibility
    {
        bool IsDirectlyCompatible = false;      // Direct 1:1 conversion possible
        bool RequiresResize = false;            // Conversion requires size adjustment
        bool RequiresReformat = false;          // Conversion requires format change
        bool RequiresReallocation = false;     // Conversion requires new GPU memory
        bool IsLossyConversion = false;         // Conversion may lose data
        f32 ConversionCost = 0.0f;             // Relative cost of conversion (0.0 = free, 1.0 = expensive)
        std::string ConversionPath;            // Description of conversion steps required
    };

    /**
     * @brief Result of a resource conversion attempt
     */
    template<typename TargetType>
    struct ConversionResult
    {
        enum class Status
        {
            Success,                // Conversion completed successfully
            Failed,                 // Conversion failed
            Incompatible,           // Source and target are incompatible
            InsufficientData,       // Not enough information to perform conversion
            MemoryError,            // Memory allocation failed during conversion
            ValidationError         // Converted resource failed validation
        };

        Status ResultStatus = Status::Failed;
        Ref<TargetType> ConvertedResource = nullptr;
        ConversionCompatibility CompatibilityInfo;
        std::string ErrorMessage;
        std::string WarningMessage;
        f32 ActualConversionTime = 0.0f;       // Time taken for conversion in milliseconds
        
        // Success check
        bool IsSuccessful() const { return ResultStatus == Status::Success && ConvertedResource != nullptr; }
        
        // Operator for easy conditional checking
        operator bool() const { return IsSuccessful(); }
    };

    /**
     * @brief Smart resource converter that handles type conversions with Hazel-style intelligence
     * 
     * This class provides intelligent resource conversion capabilities similar to Hazel's resource
     * management system, leveraging OpenGL declarations for metadata-driven conversions.
     */
    class ResourceConverter
    {
    public:
        /**
         * @brief Conversion function signature for custom converters
         */
        template<typename FromType, typename ToType>
        using ConversionFunction = std::function<ConversionResult<ToType>(const Ref<FromType>&, 
                                                                         const OpenGLResourceDeclaration::ResourceInfo*)>;

        ResourceConverter() = default;
        ~ResourceConverter() = default;

        // Disable copy semantics to prevent accidental copying of conversion registry
        ResourceConverter(const ResourceConverter&) = delete;
        ResourceConverter& operator=(const ResourceConverter&) = delete;

        /**
         * @brief Check if conversion between two types is possible
         * @tparam FromType Source resource type
         * @tparam ToType Target resource type
         * @param sourceDeclaration Optional declaration metadata for source resource
         * @param targetDeclaration Optional declaration metadata for target resource
         * @return Conversion compatibility information
         */
        template<typename FromType, typename ToType>
        ConversionCompatibility CheckConversionCompatibility(
            const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration = nullptr,
            const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration = nullptr) const;

        /**
         * @brief Attempt to convert a resource to a different type
         * @tparam FromType Source resource type
         * @tparam ToType Target resource type
         * @param source Source resource to convert
         * @param targetDeclaration Optional declaration metadata for target type
         * @param allowLossyConversion Whether to allow conversions that may lose data
         * @return Conversion result with converted resource or error information
         */
        template<typename FromType, typename ToType>
        ConversionResult<ToType> ConvertResource(
            const Ref<FromType>& source,
            const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration = nullptr,
            bool allowLossyConversion = false) const;

        /**
         * @brief Register a custom conversion function for specific types
         * @tparam FromType Source resource type
         * @tparam ToType Target resource type
         * @param converter Custom conversion function
         */
        template<typename FromType, typename ToType>
        void RegisterConverter(ConversionFunction<FromType, ToType> converter);

        /**
         * @brief Try to intelligently convert a resource based on declaration metadata
         * @tparam ToType Target resource type
         * @param sourceResource Source resource (any compatible type)
         * @param sourceType Type of the source resource
         * @param targetDeclaration Declaration metadata for target resource
         * @param registry Registry context for additional metadata
         * @return Conversion result
         */
        template<typename ToType>
        ConversionResult<ToType> SmartConvert(
            const ShaderResource& sourceResource,
            ShaderResourceType sourceType,
            const OpenGLResourceDeclaration::ResourceInfo& targetDeclaration,
            const UniformBufferRegistry& registry) const;

        /**
         * @brief Get the optimal conversion path between two resource types
         * @param fromType Source resource type
         * @param toType Target resource type
         * @param sourceDeclaration Optional source declaration metadata
         * @param targetDeclaration Optional target declaration metadata
         * @return Vector of intermediate conversion steps, empty if no path exists
         */
        std::vector<ShaderResourceType> GetConversionPath(
            ShaderResourceType fromType,
            ShaderResourceType toType,
            const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration = nullptr,
            const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration = nullptr) const;

        /**
         * @brief Estimate the cost of conversion between two types
         * @param fromType Source resource type
         * @param toType Target resource type
         * @param sourceSize Size of source resource in bytes (0 if unknown)
         * @param targetSize Size of target resource in bytes (0 if unknown)
         * @return Estimated cost (0.0 = free, 1.0 = very expensive)
         */
        f32 EstimateConversionCost(
            ShaderResourceType fromType,
            ShaderResourceType toType,
            size_t sourceSize = 0,
            size_t targetSize = 0) const;

        /**
         * @brief Check if a specific conversion path is available
         * @param fromType Source resource type
         * @param toType Target resource type
         * @return True if conversion is possible, false otherwise
         */
        bool IsConversionAvailable(ShaderResourceType fromType, ShaderResourceType toType) const;

        /**
         * @brief Get all supported conversion targets for a source type
         * @param sourceType Source resource type
         * @return Vector of compatible target types
         */
        std::vector<ShaderResourceType> GetSupportedConversions(ShaderResourceType sourceType) const;

        /**
         * @brief Initialize the converter with default conversion rules
         */
        void InitializeDefaultConverters();

        /**
         * @brief Get conversion statistics for performance monitoring
         */
        struct ConversionStatistics
        {
            u32 TotalConversions = 0;
            u32 SuccessfulConversions = 0;
            u32 FailedConversions = 0;
            f32 AverageConversionTime = 0.0f;
            f32 TotalConversionTime = 0.0f;
            std::unordered_map<std::string, u32> ConversionCounts; // "FromType->ToType" -> count
        };

        const ConversionStatistics& GetStatistics() const { return m_Statistics; }
        void ResetStatistics() { m_Statistics = {}; }

    private:
        // Internal conversion registry
        std::unordered_map<std::string, std::function<ConversionResult<void*>(const void*, const OpenGLResourceDeclaration::ResourceInfo*)>> m_Converters;
        
        // Conversion compatibility matrix
        std::unordered_map<std::string, ConversionCompatibility> m_CompatibilityMatrix;
        
        // Performance statistics
        mutable ConversionStatistics m_Statistics;

        /**
         * @brief Generate a unique key for a conversion pair
         */
        std::string GetConversionKey(std::type_index fromType, std::type_index toType) const;
        std::string GetConversionKey(ShaderResourceType fromType, ShaderResourceType toType) const;

        /**
         * @brief Initialize built-in conversion rules
         */
        void InitializeBuiltinConversions();

        /**
         * @brief Register compatibility information for a conversion pair
         */
        void RegisterCompatibility(ShaderResourceType fromType, ShaderResourceType toType, 
                                 const ConversionCompatibility& compatibility);

        /**
         * @brief Update conversion statistics
         */
        void UpdateStatistics(const std::string& conversionKey, bool success, f32 conversionTime) const;

        // Built-in conversion implementations
        ConversionResult<UniformBufferArray> ConvertToUniformBufferArray(const Ref<UniformBuffer>& source,
                                                                         const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;

        ConversionResult<StorageBufferArray> ConvertToStorageBufferArray(const Ref<StorageBuffer>& source,
                                                                         const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;

        ConversionResult<Texture2DArray> ConvertToTexture2DArray(const Ref<Texture2D>& source,
                                                                 const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;

        ConversionResult<TextureCubemapArray> ConvertToTextureCubemapArray(const Ref<TextureCubemap>& source,
                                                                           const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;

        ConversionResult<UniformBuffer> ConvertFromUniformBufferArray(const Ref<UniformBufferArray>& source,
                                                                      const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;

        ConversionResult<StorageBuffer> ConvertFromStorageBufferArray(const Ref<StorageBufferArray>& source,
                                                                      const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;

        ConversionResult<Texture2D> ConvertFromTexture2DArray(const Ref<Texture2DArray>& source,
                                                              const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;

        ConversionResult<TextureCubemap> ConvertFromTextureCubemapArray(const Ref<TextureCubemapArray>& source,
                                                                        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const;
    };

    /**
     * @brief Global resource converter instance (singleton pattern)
     */
    ResourceConverter& GetResourceConverter();
}
