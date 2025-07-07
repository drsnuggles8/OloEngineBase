#pragma once

#include "OloEngine/Core/Base.h"
#include "UniformBufferRegistry.h"
#include <type_traits>
#include <optional>
#include <functional>

namespace OloEngine
{
    // Forward declarations
    class OpenGLResourceDeclaration;
    class ResourceConverter;
    template<typename T>
    struct ConversionResult;

    /**
     * @brief Extended resource information combining registry and declaration data
     */
    struct ResourceInfoExtended
    {
        std::string Name;
        ShaderResourceType Type = ShaderResourceType::None;
        bool IsBound = false;
        bool IsValid = false;
        bool IsOptional = false;
        
        // Registry information
        u32 RegistryBinding = UINT32_MAX;
        u32 RegistrySet = UINT32_MAX;
        bool IsArray = false;
        u32 ArraySize = 0;
        
        // Declaration information (if available)
        bool HasDeclaration = false;
        u32 DeclarationBinding = UINT32_MAX;
        u32 DeclarationSet = UINT32_MAX;
        u32 EstimatedSize = 0;
        std::string AccessPattern;
        std::string UsageFrequency;
        
        // Validation status
        bool IsCompatible = true;
        std::vector<std::string> Issues;
        
        operator bool() const { return IsBound && IsValid && IsCompatible; }
    };

    /**
     * @brief Resource validation issue with declaration context
     */
    struct ResourceValidationIssue
    {
        enum class Severity { Info, Warning, Error, Critical };
        
        Severity Level = Severity::Warning;
        std::string ResourceName;
        std::string Category;        // e.g., "TypeMismatch", "BindingConflict", "MissingDeclaration"
        std::string Message;
        std::string Suggestion;
        
        // Declaration context
        std::string PassName;
        u32 ExpectedBinding = UINT32_MAX;
        u32 ActualBinding = UINT32_MAX;
        u32 ExpectedSet = UINT32_MAX;
        u32 ActualSet = UINT32_MAX;
        
        ResourceValidationIssue() = default;
        ResourceValidationIssue(Severity level, const std::string& resource, 
                              const std::string& category, const std::string& message)
            : Level(level), ResourceName(resource), Category(category), Message(message) {}
    };
    /**
     * @brief Enhanced resource access result with error information
     */
    template<typename T>
    struct ResourceAccessResult
    {
        Ref<T> Resource = nullptr;
        bool IsSuccess = false;
        std::string ErrorMessage;
        
        // Implicit conversion to bool for easy checking
        operator bool() const { return IsSuccess && Resource != nullptr; }
        
        // Implicit conversion to resource pointer
        operator Ref<T>() const { return Resource; }
        
        // Arrow operator for direct access
        T* operator->() const { return Resource.get(); }
        
        // Dereference operator
        T& operator*() const { return *Resource; }
        
        // Get the resource or a default value
        Ref<T> ValueOr(Ref<T> defaultValue) const
        {
            return IsSuccess ? Resource : defaultValue;
        }
        
        // Create success result
        static ResourceAccessResult<T> Success(Ref<T> resource)
        {
            ResourceAccessResult<T> result;
            result.Resource = resource;
            result.IsSuccess = true;
            return result;
        }
        
        // Create error result
        static ResourceAccessResult<T> Error(const std::string& message)
        {
            ResourceAccessResult<T> result;
            result.ErrorMessage = message;
            result.IsSuccess = false;
            return result;
        }
    };

    /**
     * @brief Type traits for compile-time resource validation
     */
    template<typename T>
    struct ResourceTypeTraits
    {
        static constexpr bool IsValidResourceType = false;
        static constexpr ShaderResourceType Type = ShaderResourceType::None;
        static constexpr const char* TypeName = "Unknown";
    };

    // Specialize for supported resource types
    template<>
    struct ResourceTypeTraits<UniformBuffer>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::UniformBuffer;
        static constexpr const char* TypeName = "UniformBuffer";
    };

    template<>
    struct ResourceTypeTraits<StorageBuffer>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::StorageBuffer;
        static constexpr const char* TypeName = "StorageBuffer";
    };

    template<>
    struct ResourceTypeTraits<Texture2D>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::Texture2D;
        static constexpr const char* TypeName = "Texture2D";
    };

    template<>
    struct ResourceTypeTraits<TextureCubemap>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::TextureCube;
        static constexpr const char* TypeName = "TextureCubemap";
    };

    template<>
    struct ResourceTypeTraits<UniformBufferArray>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::UniformBufferArray;
        static constexpr const char* TypeName = "UniformBufferArray";
    };

    template<>
    struct ResourceTypeTraits<StorageBufferArray>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::StorageBufferArray;
        static constexpr const char* TypeName = "StorageBufferArray";
    };

    template<>
    struct ResourceTypeTraits<Texture2DArray>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::Texture2DArray;
        static constexpr const char* TypeName = "Texture2DArray";
    };

    template<>
    struct ResourceTypeTraits<TextureCubemapArray>
    {
        static constexpr bool IsValidResourceType = true;
        static constexpr ShaderResourceType Type = ShaderResourceType::TextureCubeArray;
        static constexpr const char* TypeName = "TextureCubemapArray";
    };

    /**
     * @brief Smart resource converter for automatic type conversions
     */
    class ResourceConverter
    {
    public:
        /**
         * @brief Check if conversion between types is possible
         * @tparam From Source type
         * @tparam To Destination type
         * @return True if conversion is possible
         */
        template<typename From, typename To>
        static constexpr bool CanConvert()
        {
            // Same type conversion is always possible
            if constexpr (std::is_same_v<From, To>)
                return true;
            
            // UniformBuffer -> UniformBufferArray conversion
            if constexpr (std::is_same_v<From, UniformBuffer> && std::is_same_v<To, UniformBufferArray>)
                return true;
                
            // StorageBuffer -> StorageBufferArray conversion
            if constexpr (std::is_same_v<From, StorageBuffer> && std::is_same_v<To, StorageBufferArray>)
                return true;
                
            // Texture2D -> Texture2DArray conversion
            if constexpr (std::is_same_v<From, Texture2D> && std::is_same_v<To, Texture2DArray>)
                return true;
                
            // TextureCubemap -> TextureCubemapArray conversion
            if constexpr (std::is_same_v<From, TextureCubemap> && std::is_same_v<To, TextureCubemapArray>)
                return true;

            return false;
        }

        /**
         * @brief Convert resource from one type to another
         * @tparam From Source type
         * @tparam To Destination type
         * @param resource Source resource
         * @return Converted resource or nullptr if conversion failed
         */
        template<typename From, typename To>
        static Ref<To> Convert(const Ref<From>& resource)
        {
            static_assert(CanConvert<From, To>(), "Resource conversion not supported");
            
            if (!resource)
                return nullptr;
            
            // Same type conversion
            if constexpr (std::is_same_v<From, To>)
            {
                return resource;
            }
            // UniformBuffer -> UniformBufferArray
            else if constexpr (std::is_same_v<From, UniformBuffer> && std::is_same_v<To, UniformBufferArray>)
            {
                auto array = CreateRef<UniformBufferArray>("converted_array", 0, 1);
                array->SetResource(0, resource);
                return array;
            }
            // StorageBuffer -> StorageBufferArray
            else if constexpr (std::is_same_v<From, StorageBuffer> && std::is_same_v<To, StorageBufferArray>)
            {
                auto array = CreateRef<StorageBufferArray>("converted_array", 0, 1);
                array->SetResource(0, resource);
                return array;
            }
            // Texture2D -> Texture2DArray
            else if constexpr (std::is_same_v<From, Texture2D> && std::is_same_v<To, Texture2DArray>)
            {
                auto array = CreateRef<Texture2DArray>("converted_array", 0, 1);
                array->SetResource(0, resource);
                return array;
            }
            // TextureCubemap -> TextureCubemapArray
            else if constexpr (std::is_same_v<From, TextureCubemap> && std::is_same_v<To, TextureCubemapArray>)
            {
                auto array = CreateRef<TextureCubemapArray>("converted_array", 0, 1);
                array->SetResource(0, resource);
                return array;
            }
            
            return nullptr;
        }
    };

    /**
     * @brief Resource availability checker
     */
    class ResourceAvailabilityChecker
    {
    public:
        enum class AvailabilityStatus
        {
            Available,
            NotBound,
            TypeMismatch,
            Invalid,
            Missing
        };

        struct AvailabilityInfo
        {
            AvailabilityStatus Status = AvailabilityStatus::Missing;
            std::string Message;
            ShaderResourceType ExpectedType = ShaderResourceType::None;
            ShaderResourceType ActualType = ShaderResourceType::None;
            bool IsArray = false;
            
            operator bool() const { return Status == AvailabilityStatus::Available; }
        };

        /**
         * @brief Check resource availability in registry
         * @tparam T Expected resource type
         * @param registry Registry to check
         * @param name Resource name
         * @return Availability information
         */
        template<typename T>
        static AvailabilityInfo CheckAvailability(const UniformBufferRegistry& registry, const std::string& name)
        {
            static_assert(ResourceTypeTraits<T>::IsValidResourceType, "T must be a valid resource type");
            
            AvailabilityInfo info;
            info.ExpectedType = ResourceTypeTraits<T>::Type;
            
            // Check if resource is declared in shader
            const auto* binding = registry.GetResourceBinding(name);
            if (!binding)
            {
                info.Status = AvailabilityStatus::Missing;
                info.Message = "Resource '" + name + "' is not declared in shader";
                return info;
            }
            
            info.ActualType = binding->Type;
            info.IsArray = binding->IsArray;
            
            // Check type compatibility
            if (binding->Type != info.ExpectedType)
            {
                // Check if conversion is possible
                if (!IsConvertibleType(binding->Type, info.ExpectedType))
                {
                    info.Status = AvailabilityStatus::TypeMismatch;
                    info.Message = "Type mismatch for resource '" + name + "': expected " + 
                                 ResourceTypeTraits<T>::TypeName + ", found " + 
                                 GetResourceTypeName(binding->Type);
                    return info;
                }
            }
            
            // Check if resource is bound
            if (!registry.IsResourceBound(name))
            {
                info.Status = AvailabilityStatus::NotBound;
                info.Message = "Resource '" + name + "' is declared but not bound";
                return info;
            }
            
            // Check if bound resource is valid
            auto resource = registry.GetResource<T>(name);
            if (!resource)
            {
                info.Status = AvailabilityStatus::Invalid;
                info.Message = "Resource '" + name + "' is bound but invalid";
                return info;
            }
            
            info.Status = AvailabilityStatus::Available;
            info.Message = "Resource '" + name + "' is available";
            return info;
        }

    private:
        static bool IsConvertibleType(ShaderResourceType from, ShaderResourceType to);
        static const char* GetResourceTypeName(ShaderResourceType type);
    };

    /**
     * @brief Enhanced template getter with comprehensive error handling and smart conversions
     */
    class EnhancedResourceGetter
    {
    public:
        /**
         * @brief Get resource with enhanced error handling using OpenGL declarations
         * @tparam T Expected resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param passName Optional pass name for OpenGL declaration lookup
         * @return Resource access result with error information
         */
        template<typename T>
        static ResourceAccessResult<T> GetResource(const UniformBufferRegistry& registry, 
                                                 const std::string& name, 
                                                 const std::string& passName = "")
        {
            static_assert(ResourceTypeTraits<T>::IsValidResourceType, 
                         "T must be a valid resource type (UniformBuffer, Texture2D, etc.)");
            
            // First, try to get information from OpenGL declarations if available
            const auto* openglDeclaration = registry.GetOpenGLDeclaration(passName);
            if (openglDeclaration)
            {
                auto declarationResult = GetResourceFromDeclaration<T>(registry, name, *openglDeclaration);
                if (declarationResult)
                {
                    return declarationResult;
                }
                // If declaration lookup failed, continue with traditional method as fallback
            }
            
            // Check availability using traditional method
            auto availability = ResourceAvailabilityChecker::CheckAvailability<T>(registry, name);
            if (!availability)
            {
                return ResourceAccessResult<T>::Error(availability.Message);
            }
            
            // Try direct access first
            auto resource = registry.GetResource<T>(name);
            if (resource)
            {
                return ResourceAccessResult<T>::Success(resource);
            }
            
            // Try intelligent conversion with declaration context if direct access failed
            return TryIntelligentConversion<T>(registry, name, passName);
        }

        /**
         * @brief Get resource using OpenGL declaration metadata for enhanced error reporting
         * @tparam T Expected resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param declaration OpenGL resource declaration
         * @return Resource access result with declaration-enhanced error information
         */
        template<typename T>
        static ResourceAccessResult<T> GetResourceFromDeclaration(const UniformBufferRegistry& registry,
                                                                 const std::string& name,
                                                                 const OpenGLResourceDeclaration& declaration)
        {
            static_assert(ResourceTypeTraits<T>::IsValidResourceType, 
                         "T must be a valid resource type");
            
            // Check if resource exists in declaration
            const auto* resourceInfo = declaration.GetResource(name);
            if (!resourceInfo)
            {
                return ResourceAccessResult<T>::Error(
                    "Resource '" + name + "' not found in OpenGL declaration '" + 
                    declaration.GetDeclaration().PassName + "'");
            }
            
            // Validate type compatibility with declaration metadata
            ShaderResourceType expectedType = ResourceTypeTraits<T>::Type;
            if (resourceInfo->Type != expectedType)
            {
                // Check if smart conversion is possible
                if (!CanConvertWithDeclaration(*resourceInfo, expectedType))
                {
                    return ResourceAccessResult<T>::Error(
                        "Type mismatch in declaration for '" + name + "': expected " + 
                        ResourceTypeTraits<T>::TypeName + ", declared as " + 
                        GetResourceTypeName(resourceInfo->Type) + 
                        " (binding=" + std::to_string(resourceInfo->Binding) + 
                        ", set=" + std::to_string(resourceInfo->Set) + ")");
                }
            }
            
            // Check if resource is marked as optional
            if (resourceInfo->IsOptional)
            {
                // For optional resources, try to get it but don't fail if not found
                auto resource = registry.GetResource<T>(name);
                if (!resource)
                {
                    return ResourceAccessResult<T>::Error(
                        "Optional resource '" + name + "' is not bound (this may be acceptable)");
                }
                return ResourceAccessResult<T>::Success(resource);
            }
            
            // Try to get the resource with declaration-enhanced error reporting
            auto resource = registry.GetResource<T>(name);
            if (!resource)
            {
                // Try smart conversion with declaration context
                auto convertedResult = TrySmartConversionWithDeclaration<T>(registry, name, *resourceInfo);
                if (convertedResult)
                {
                    return convertedResult;
                }
                
                return ResourceAccessResult<T>::Error(
                    "Resource '" + name + "' declared in OpenGL declaration but not bound in registry. " +
                    "Expected type: " + ResourceTypeTraits<T>::TypeName + 
                    ", Access pattern: " + GetAccessPatternName(resourceInfo->Access) +
                    ", Frequency: " + GetUsageFrequencyName(resourceInfo->Frequency) +
                    ", Binding: " + std::to_string(resourceInfo->Binding) +
                    ", Set: " + std::to_string(resourceInfo->Set));
            }
            
            return ResourceAccessResult<T>::Success(resource);
        }

        /**
         * @brief Get resource with fallback and declaration context
         * @tparam T Expected resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param fallback Fallback resource if not found
         * @param passName Optional pass name for declaration context
         * @return Resource or fallback
         */
        template<typename T>
        static Ref<T> GetResourceOrFallback(const UniformBufferRegistry& registry, 
                                           const std::string& name, 
                                           Ref<T> fallback,
                                           const std::string& passName = "")
        {
            auto result = GetResource<T>(registry, name, passName);
            return result.ValueOr(fallback);
        }

        /**
         * @brief Get resource with factory function and declaration context
         * @tparam T Expected resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param factory Factory function to create resource if missing
         * @param passName Optional pass name for declaration context
         * @return Resource (existing or newly created)
         */
        template<typename T>
        static Ref<T> GetOrCreateResource(UniformBufferRegistry& registry, 
                                        const std::string& name, 
                                        std::function<Ref<T>()> factory,
                                        const std::string& passName = "")
        {
            auto result = GetResource<T>(registry, name, passName);
            if (result)
                return result.Resource;
            
            // Create new resource using factory
            if (factory)
            {
                auto newResource = factory();
                if (newResource)
                {
                    // Try to bind the new resource with declaration context
                    if (BindResourceWithDeclaration(registry, name, newResource, passName))
                    {
                        return newResource;
                    }
                }
            }
            
            return nullptr;
        }

        /**
         * @brief Create resource using OpenGL declaration metadata
         * @tparam T Resource type to create
         * @param registry Registry to bind resource to
         * @param name Resource name
         * @param declaration OpenGL declaration with resource metadata
         * @return Created and bound resource, or nullptr if creation failed
         */
        template<typename T>
        static Ref<T> CreateResourceFromDeclaration(UniformBufferRegistry& registry,
                                                   const std::string& name,
                                                   const OpenGLResourceDeclaration& declaration)
        {
            static_assert(ResourceTypeTraits<T>::IsValidResourceType, 
                         "T must be a valid resource type");
            
            const auto* resourceInfo = declaration.GetResource(name);
            if (!resourceInfo)
                return nullptr;
            
            // Validate type compatibility
            if (resourceInfo->Type != ResourceTypeTraits<T>::Type)
                return nullptr;
            
            Ref<T> resource = nullptr;
            
            // Create resource based on type and declaration metadata
            if constexpr (std::is_same_v<T, UniformBuffer>)
            {
                if (resourceInfo->Size > 0)
                {
                    resource = CreateRef<UniformBuffer>(resourceInfo->Size);
                }
            }
            else if constexpr (std::is_same_v<T, StorageBuffer>)
            {
                if (resourceInfo->Size > 0)
                {
                    BufferUsage usage = (resourceInfo->Access == OpenGLResourceDeclaration::AccessPattern::ReadOnly) 
                                      ? BufferUsage::Static : BufferUsage::Dynamic;
                    resource = CreateRef<StorageBuffer>(resourceInfo->Size, usage);
                }
            }
            else if constexpr (std::is_same_v<T, UniformBufferArray>)
            {
                if (resourceInfo->IsArray && resourceInfo->ArraySize > 0)
                {
                    resource = CreateRef<UniformBufferArray>(name, resourceInfo->Binding, resourceInfo->ArraySize);
                }
            }
            else if constexpr (std::is_same_v<T, StorageBufferArray>)
            {
                if (resourceInfo->IsArray && resourceInfo->ArraySize > 0)
                {
                    resource = CreateRef<StorageBufferArray>(name, resourceInfo->Binding, resourceInfo->ArraySize);
                }
            }
            // Note: Texture creation requires more complex parameters (width, height, format, etc.)
            // which are not typically available in shader declarations
            
            // Bind the created resource if successful
            if (resource)
            {
                ShaderResourceInput input(resource);
                input.WithSet(resourceInfo->Set);
                if (resourceInfo->Binding != UINT32_MAX)
                {
                    input.WithBinding(resourceInfo->Binding);
                }
                input.WithDebugName(resourceInfo->Name);
                
                if (registry.SetResource(name, input))
                {
                    return resource;
                }
            }
            
            return nullptr;
        }

        /**
         * @brief Check if resource is available and ready using declaration context
         * @tparam T Expected resource type
         * @param registry Registry to check
         * @param name Resource name
         * @param passName Optional pass name for declaration context
         * @return True if resource is available and valid
         */
        template<typename T>
        static bool IsResourceReady(const UniformBufferRegistry& registry, 
                                  const std::string& name,
                                  const std::string& passName = "")
        {
            // First check with declaration context if available
            const auto* openglDeclaration = registry.GetOpenGLDeclaration(passName);
            if (openglDeclaration)
            {
                const auto* resourceInfo = openglDeclaration->GetResource(name);
                if (!resourceInfo)
                    return false;
                
                // If resource is optional and not bound, it's still considered "ready"
                if (resourceInfo->IsOptional && !registry.IsResourceBound(name))
                    return true;
            }
            
            // Use traditional availability check
            auto availability = ResourceAvailabilityChecker::CheckAvailability<T>(registry, name);
            return availability.Status == ResourceAvailabilityChecker::AvailabilityStatus::Available;
        }

        /**
         * @brief Get comprehensive resource information using declaration context
         * @param registry Registry to query
         * @param name Resource name
         * @param passName Optional pass name for declaration context
         * @return Detailed resource information including declaration metadata
         */
        static ResourceInfoExtended GetResourceInfo(const UniformBufferRegistry& registry,
                                                   const std::string& name,
                                                   const std::string& passName = "");

        /**
         * @brief Validate all resources against their declarations
         * @param registry Registry to validate
         * @param passName Optional pass name for declaration context
         * @return Vector of validation issues found
         */
        static std::vector<ResourceValidationIssue> ValidateResourcesAgainstDeclarations(
            const UniformBufferRegistry& registry,
            const std::string& passName = "");

    private:
        /**
         * @brief Try intelligent resource conversion using the new ResourceConverter system
         * @tparam T Target resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param passName Pass name for declaration context
         * @return Converted resource or detailed error information
         */
        template<typename T>
        static ResourceAccessResult<T> TryIntelligentConversion(
            const UniformBufferRegistry& registry, 
            const std::string& name,
            const std::string& passName = "")
        {
            const auto* binding = registry.GetResourceBinding(name);
            if (!binding)
            {
                return ResourceAccessResult<T>::Error("Resource binding not found for intelligent conversion");
            }

            // Get declaration context for enhanced conversion
            const OpenGLResourceDeclaration::ResourceInfo* declarationInfo = nullptr;
            if (!passName.empty())
            {
                const auto* declaration = registry.GetOpenGLDeclaration(passName);
                if (declaration)
                {
                    declarationInfo = declaration->GetResource(name);
                }
            }

            // Use the new ResourceConverter for intelligent conversion
            auto& converter = GetResourceConverter();
            
            // Try conversion from the bound resource type to target type
            switch (binding->Type)
            {
                case ShaderResourceType::UniformBuffer:
                {
                    if constexpr (std::is_same_v<T, UniformBufferArray>)
                    {
                        auto buffer = registry.GetResource<UniformBuffer>(name);
                        if (buffer)
                        {
                            auto conversionResult = converter.ConvertResource<UniformBuffer, UniformBufferArray>(
                                buffer, declarationInfo, false);
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource, 
                                    "Successfully converted UniformBuffer to UniformBufferArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                case ShaderResourceType::StorageBuffer:
                {
                    if constexpr (std::is_same_v<T, StorageBufferArray>)
                    {
                        auto buffer = registry.GetResource<StorageBuffer>(name);
                        if (buffer)
                        {
                            auto conversionResult = converter.ConvertResource<StorageBuffer, StorageBufferArray>(
                                buffer, declarationInfo, false);
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource,
                                    "Successfully converted StorageBuffer to StorageBufferArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                case ShaderResourceType::Texture2D:
                {
                    if constexpr (std::is_same_v<T, Texture2DArray>)
                    {
                        auto texture = registry.GetResource<Texture2D>(name);
                        if (texture)
                        {
                            auto conversionResult = converter.ConvertResource<Texture2D, Texture2DArray>(
                                texture, declarationInfo, false);
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource,
                                    "Successfully converted Texture2D to Texture2DArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                case ShaderResourceType::TextureCube:
                {
                    if constexpr (std::is_same_v<T, TextureCubemapArray>)
                    {
                        auto texture = registry.GetResource<TextureCubemap>(name);
                        if (texture)
                        {
                            auto conversionResult = converter.ConvertResource<TextureCubemap, TextureCubemapArray>(
                                texture, declarationInfo, false);
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource,
                                    "Successfully converted TextureCubemap to TextureCubemapArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                // Reverse conversions (array to single)
                case ShaderResourceType::UniformBufferArray:
                {
                    if constexpr (std::is_same_v<T, UniformBuffer>)
                    {
                        auto bufferArray = registry.GetResource<UniformBufferArray>(name);
                        if (bufferArray)
                        {
                            auto conversionResult = converter.ConvertResource<UniformBufferArray, UniformBuffer>(
                                bufferArray, declarationInfo, true); // Allow lossy conversion for array to single
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource,
                                    "Successfully extracted UniformBuffer from UniformBufferArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                case ShaderResourceType::StorageBufferArray:
                {
                    if constexpr (std::is_same_v<T, StorageBuffer>)
                    {
                        auto bufferArray = registry.GetResource<StorageBufferArray>(name);
                        if (bufferArray)
                        {
                            auto conversionResult = converter.ConvertResource<StorageBufferArray, StorageBuffer>(
                                bufferArray, declarationInfo, true);
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource,
                                    "Successfully extracted StorageBuffer from StorageBufferArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                case ShaderResourceType::Texture2DArray:
                {
                    if constexpr (std::is_same_v<T, Texture2D>)
                    {
                        auto textureArray = registry.GetResource<Texture2DArray>(name);
                        if (textureArray)
                        {
                            auto conversionResult = converter.ConvertResource<Texture2DArray, Texture2D>(
                                textureArray, declarationInfo, true);
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource,
                                    "Successfully extracted Texture2D from Texture2DArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                case ShaderResourceType::TextureCubeArray:
                {
                    if constexpr (std::is_same_v<T, TextureCubemap>)
                    {
                        auto textureArray = registry.GetResource<TextureCubemapArray>(name);
                        if (textureArray)
                        {
                            auto conversionResult = converter.ConvertResource<TextureCubemapArray, TextureCubemap>(
                                textureArray, declarationInfo, true);
                            if (conversionResult.IsSuccessful())
                            {
                                return ResourceAccessResult<T>::Success(conversionResult.ConvertedResource,
                                    "Successfully extracted TextureCubemap from TextureCubemapArray using intelligent conversion");
                            }
                            else
                            {
                                return ResourceAccessResult<T>::Error(
                                    "Intelligent conversion failed: " + conversionResult.ErrorMessage);
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
            }

            return ResourceAccessResult<T>::Error(
                "No intelligent conversion available from " + 
                std::string(ResourceAvailabilityChecker::GetResourceTypeName(binding->Type)) +
                " to requested type");
        }

        /**
         * @brief Get resource with automatic intelligent conversion fallback
         * @tparam T Target resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param passName Pass name for declaration context
         * @return Resource access result with intelligent conversion if needed
         */
        template<typename T>
        static ResourceAccessResult<T> GetResourceWithIntelligentConversion(
            const UniformBufferRegistry& registry, 
            const std::string& name, 
            const std::string& passName = "")
        {
            // First try direct access
            auto directResult = GetResource<T>(registry, name, passName);
            if (directResult.IsSuccessful())
            {
                return directResult;
            }

            // If direct access failed, try intelligent conversion
            auto conversionResult = TryIntelligentConversion<T>(registry, name, passName);
            if (conversionResult.IsSuccessful())
            {
                // Add note about conversion
                conversionResult.Message += " (automatic intelligent conversion applied)";
                return conversionResult;
            }

            // Return the original error if conversion also failed
            return directResult;
        }

        /**
         * @brief Try smart conversion with declaration context
         * @tparam T Target resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param resourceInfo Declaration resource information
         * @return Converted resource or error
         */
        template<typename T>
        static ResourceAccessResult<T> TrySmartConversionWithDeclaration(
            const UniformBufferRegistry& registry, 
            const std::string& name,
            const OpenGLResourceDeclaration::ResourceInfo& resourceInfo)
        {
            // Enhanced conversion with declaration metadata
            switch (resourceInfo.Type)
            {
                case ShaderResourceType::UniformBuffer:
                {
                    if constexpr (std::is_same_v<T, UniformBufferArray>)
                    {
                        auto buffer = registry.GetResource<UniformBuffer>(name);
                        if (buffer)
                        {
                            // Use declaration metadata for better array creation
                            auto array = CreateRef<UniformBufferArray>(
                                name + "_converted", 
                                resourceInfo.Binding,
                                resourceInfo.IsArray ? resourceInfo.ArraySize : 1
                            );
                            array->SetResource(0, buffer);
                            return ResourceAccessResult<T>::Success(
                                std::static_pointer_cast<T>(array)
                            );
                        }
                    }
                    break;
                }
                case ShaderResourceType::StorageBuffer:
                {
                    if constexpr (std::is_same_v<T, StorageBufferArray>)
                    {
                        auto buffer = registry.GetResource<StorageBuffer>(name);
                        if (buffer)
                        {
                            auto array = CreateRef<StorageBufferArray>(
                                name + "_converted",
                                resourceInfo.Binding, 
                                resourceInfo.IsArray ? resourceInfo.ArraySize : 1
                            );
                            array->SetResource(0, buffer);
                            return ResourceAccessResult<T>::Success(
                                std::static_pointer_cast<T>(array)
                            );
                        }
                    }
                    break;
                }
                case ShaderResourceType::Texture2D:
                {
                    if constexpr (std::is_same_v<T, Texture2DArray>)
                    {
                        auto texture = registry.GetResource<Texture2D>(name);
                        if (texture)
                        {
                            auto array = CreateRef<Texture2DArray>(
                                name + "_converted",
                                resourceInfo.Binding,
                                resourceInfo.IsArray ? resourceInfo.ArraySize : 1
                            );
                            array->SetResource(0, texture);
                            return ResourceAccessResult<T>::Success(
                                std::static_pointer_cast<T>(array)
                            );
                        }
                    }
                    break;
                }
                case ShaderResourceType::TextureCube:
                {
                    if constexpr (std::is_same_v<T, TextureCubemapArray>)
                    {
                        auto texture = registry.GetResource<TextureCubemap>(name);
                        if (texture)
                        {
                            auto array = CreateRef<TextureCubemapArray>(
                                name + "_converted",
                                resourceInfo.Binding,
                                resourceInfo.IsArray ? resourceInfo.ArraySize : 1
                            );
                            array->SetResource(0, texture);
                            return ResourceAccessResult<T>::Success(
                                std::static_pointer_cast<T>(array)
                            );
                        }
                    }
                    break;
                }
                default:
                    break;
            }
            
            return ResourceAccessResult<T>::Error(
                "No viable conversion found for resource '" + name + "' with declaration context. " +
                "Declared type: " + GetResourceTypeName(resourceInfo.Type) + 
                ", Requested type: " + ResourceTypeTraits<T>::TypeName +
                ", Binding: " + std::to_string(resourceInfo.Binding) +
                ", Set: " + std::to_string(resourceInfo.Set)
            );
        }

        /**
         * @brief Bind resource with declaration context for enhanced validation
         */
        template<typename T>
        static bool BindResourceWithDeclaration(UniformBufferRegistry& registry,
                                               const std::string& name,
                                               const Ref<T>& resource,
                                               const std::string& passName)
        {
            const auto* declaration = registry.GetOpenGLDeclaration(passName);
            if (declaration)
            {
                const auto* resourceInfo = declaration->GetResource(name);
                if (resourceInfo)
                {
                    // Create input with declaration metadata
                    ShaderResourceInput input(resource);
                    input.WithSet(resourceInfo->Set);
                    if (resourceInfo->Binding != UINT32_MAX)
                    {
                        input.WithBinding(resourceInfo->Binding);
                    }
                    input.WithDebugName(name);
                    input.AsOptional(resourceInfo->IsOptional);
                    
                    // Set priority based on usage frequency
                    UpdatePriority priority = UpdatePriority::Normal;
                    switch (resourceInfo->Frequency)
                    {
                        case OpenGLResourceDeclaration::UsageFrequency::Constant:
                            priority = UpdatePriority::Immediate;
                            break;
                        case OpenGLResourceDeclaration::UsageFrequency::Frequent:
                            priority = UpdatePriority::High;
                            break;
                        case OpenGLResourceDeclaration::UsageFrequency::Normal:
                            priority = UpdatePriority::Normal;
                            break;
                        case OpenGLResourceDeclaration::UsageFrequency::Rare:
                            priority = UpdatePriority::Low;
                            break;
                        case OpenGLResourceDeclaration::UsageFrequency::Never:
                            priority = UpdatePriority::Background;
                            break;
                    }
                    input.WithPriority(priority);
                    
                    return registry.SetResource(name, input);
                }
            }
            
            // Fallback to traditional binding
            return registry.SetResource(name, resource);
        }

        /**
         * @brief Check if conversion is possible with declaration metadata
         */
        static bool CanConvertWithDeclaration(const OpenGLResourceDeclaration::ResourceInfo& resourceInfo,
                                             ShaderResourceType targetType);

        /**
         * @brief Get human-readable access pattern name
         */
        static const char* GetAccessPatternName(OpenGLResourceDeclaration::AccessPattern pattern);

        /**
         * @brief Get human-readable usage frequency name
         */
        static const char* GetUsageFrequencyName(OpenGLResourceDeclaration::UsageFrequency frequency);

        /**
         * @brief Try smart conversion from bound resource to requested type (fallback method)
         * @tparam T Target resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @return Converted resource or error
         */
        template<typename T>
        static ResourceAccessResult<T> TrySmartConversion(const UniformBufferRegistry& registry, const std::string& name)
        {
            const auto* binding = registry.GetResourceBinding(name);
            if (!binding)
            {
                return ResourceAccessResult<T>::Error("Resource binding not found for conversion");
            }
            
            // Try conversions based on the bound resource type
            switch (binding->Type)
            {
                case ShaderResourceType::UniformBuffer:
                {
                    if constexpr (std::is_same_v<T, UniformBufferArray>)
                    {
                        auto buffer = registry.GetResource<UniformBuffer>(name);
                        if (buffer)
                        {
                            auto converted = ResourceConverter::Convert<UniformBuffer, UniformBufferArray>(buffer);
                            if (converted)
                                return ResourceAccessResult<T>::Success(converted);
                        }
                    }
                    break;
                }
                case ShaderResourceType::StorageBuffer:
                {
                    if constexpr (std::is_same_v<T, StorageBufferArray>)
                    {
                        auto buffer = registry.GetResource<StorageBuffer>(name);
                        if (buffer)
                        {
                            auto converted = ResourceConverter::Convert<StorageBuffer, StorageBufferArray>(buffer);
                            if (converted)
                                return ResourceAccessResult<T>::Success(converted);
                        }
                    }
                    break;
                }
                case ShaderResourceType::Texture2D:
                {
                    if constexpr (std::is_same_v<T, Texture2DArray>)
                    {
                        auto texture = registry.GetResource<Texture2D>(name);
                        if (texture)
                        {
                            auto converted = ResourceConverter::Convert<Texture2D, Texture2DArray>(texture);
                            if (converted)
                                return ResourceAccessResult<T>::Success(converted);
                        }
                    }
                    break;
                }
                case ShaderResourceType::TextureCube:
                {
                    if constexpr (std::is_same_v<T, TextureCubemapArray>)
                    {
                        auto texture = registry.GetResource<TextureCubemap>(name);
                        if (texture)
                        {
                            auto converted = ResourceConverter::Convert<TextureCubemap, TextureCubemapArray>(texture);
                            if (converted)
                                return ResourceAccessResult<T>::Success(converted);
                        }
                    }
                    break;
                }
                default:
                    break;
            }
            
            return ResourceAccessResult<T>::Error("No viable conversion found for resource '" + name + "'");
        }
    };
}
