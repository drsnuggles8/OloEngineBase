#include "OloEnginePCH.h"
#include "EnhancedResourceGetter.h"
#include "OloEngine/Core/Log.h"

namespace OloEngine
{
    bool ResourceAvailabilityChecker::IsConvertibleType(ShaderResourceType from, ShaderResourceType to)
    {
        // Same type is always convertible
        if (from == to)
            return true;
        
        // Check specific conversion rules
        switch (from)
        {
            case ShaderResourceType::UniformBuffer:
                return to == ShaderResourceType::UniformBufferArray;
                
            case ShaderResourceType::StorageBuffer:
                return to == ShaderResourceType::StorageBufferArray;
                
            case ShaderResourceType::Texture2D:
                return to == ShaderResourceType::Texture2DArray;
                
            case ShaderResourceType::TextureCube:
                return to == ShaderResourceType::TextureCubeArray;
                
            default:
                return false;
        }
    }

    const char* ResourceAvailabilityChecker::GetResourceTypeName(ShaderResourceType type)
    {
        switch (type)
        {
            case ShaderResourceType::None:
                return "None";
            case ShaderResourceType::UniformBuffer:
                return "UniformBuffer";
            case ShaderResourceType::StorageBuffer:
                return "StorageBuffer";
            case ShaderResourceType::Texture2D:
                return "Texture2D";
            case ShaderResourceType::TextureCube:
                return "TextureCube";
            case ShaderResourceType::Image2D:
                return "Image2D";
            case ShaderResourceType::UniformBufferArray:
                return "UniformBufferArray";
            case ShaderResourceType::StorageBufferArray:
                return "StorageBufferArray";
            case ShaderResourceType::Texture2DArray:
                return "Texture2DArray";
            case ShaderResourceType::TextureCubeArray:
                return "TextureCubeArray";
            default:
                return "Unknown";
        }
    }
}
