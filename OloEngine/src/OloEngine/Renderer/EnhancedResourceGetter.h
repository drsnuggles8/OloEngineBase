#pragma once

#include "OloEngine/Core/Base.h"
#include "UniformBufferRegistry.h"
#include <type_traits>
#include <optional>
#include <functional>

namespace OloEngine
{
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
         * @brief Get resource with enhanced error handling
         * @tparam T Expected resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @return Resource access result with error information
         */
        template<typename T>
        static ResourceAccessResult<T> GetResource(const UniformBufferRegistry& registry, const std::string& name)
        {
            static_assert(ResourceTypeTraits<T>::IsValidResourceType, 
                         "T must be a valid resource type (UniformBuffer, Texture2D, etc.)");
            
            // Check availability first
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
            
            // Try smart conversion if direct access failed
            return TrySmartConversion<T>(registry, name);
        }

        /**
         * @brief Get resource with fallback
         * @tparam T Expected resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param fallback Fallback resource if not found
         * @return Resource or fallback
         */
        template<typename T>
        static Ref<T> GetResourceOrFallback(const UniformBufferRegistry& registry, 
                                           const std::string& name, 
                                           Ref<T> fallback)
        {
            auto result = GetResource<T>(registry, name);
            return result.ValueOr(fallback);
        }

        /**
         * @brief Get resource with factory function for missing resources
         * @tparam T Expected resource type
         * @param registry Registry to get resource from
         * @param name Resource name
         * @param factory Factory function to create resource if missing
         * @return Resource (existing or newly created)
         */
        template<typename T>
        static Ref<T> GetOrCreateResource(UniformBufferRegistry& registry, 
                                        const std::string& name, 
                                        std::function<Ref<T>()> factory)
        {
            auto result = GetResource<T>(registry, name);
            if (result)
                return result.Resource;
            
            // Create new resource using factory
            if (factory)
            {
                auto newResource = factory();
                if (newResource)
                {
                    // Try to bind the new resource
                    if (registry.SetResource(name, newResource))
                    {
                        return newResource;
                    }
                }
            }
            
            return nullptr;
        }

        /**
         * @brief Check if resource is available and ready to use
         * @tparam T Expected resource type
         * @param registry Registry to check
         * @param name Resource name
         * @return True if resource is available and valid
         */
        template<typename T>
        static bool IsResourceReady(const UniformBufferRegistry& registry, const std::string& name)
        {
            auto availability = ResourceAvailabilityChecker::CheckAvailability<T>(registry, name);
            return availability.Status == ResourceAvailabilityChecker::AvailabilityStatus::Available;
        }

    private:
        /**
         * @brief Try smart conversion from bound resource to requested type
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
