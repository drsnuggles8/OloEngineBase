#include "OloEnginePCH.h"
#include "ResourceConverter.h"
#include "UniformBufferRegistry.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/Texture.h"
#include <chrono>
#include <sstream>

namespace OloEngine
{
    // Template specializations need to be defined here

    // CheckConversionCompatibility specializations
    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<UniformBuffer, UniformBufferArray>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.RequiresReallocation = true;
        compatibility.ConversionCost = 0.3f; // Moderate cost due to array creation
        compatibility.ConversionPath = "UniformBuffer -> UniformBufferArray (wrap in array)";
        return compatibility;
    }

    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<StorageBuffer, StorageBufferArray>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.RequiresReallocation = true;
        compatibility.ConversionCost = 0.3f;
        compatibility.ConversionPath = "StorageBuffer -> StorageBufferArray (wrap in array)";
        return compatibility;
    }

    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<Texture2D, Texture2DArray>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.RequiresReallocation = true;
        compatibility.ConversionCost = 0.4f; // Higher cost for textures
        compatibility.ConversionPath = "Texture2D -> Texture2DArray (wrap in array)";
        return compatibility;
    }

    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<TextureCubemap, TextureCubemapArray>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.RequiresReallocation = true;
        compatibility.ConversionCost = 0.5f; // Higher cost for cubemaps
        compatibility.ConversionPath = "TextureCubemap -> TextureCubemapArray (wrap in array)";
        return compatibility;
    }

    // Reverse conversions (array to single)
    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<UniformBufferArray, UniformBuffer>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.IsLossyConversion = true; // May lose array elements beyond index 0
        compatibility.ConversionCost = 0.1f; // Low cost, just extract element
        compatibility.ConversionPath = "UniformBufferArray -> UniformBuffer (extract first element)";
        return compatibility;
    }

    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<StorageBufferArray, StorageBuffer>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.IsLossyConversion = true;
        compatibility.ConversionCost = 0.1f;
        compatibility.ConversionPath = "StorageBufferArray -> StorageBuffer (extract first element)";
        return compatibility;
    }

    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<Texture2DArray, Texture2D>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.IsLossyConversion = true;
        compatibility.ConversionCost = 0.2f;
        compatibility.ConversionPath = "Texture2DArray -> Texture2D (extract first layer)";
        return compatibility;
    }

    template<>
    ConversionCompatibility ResourceConverter::CheckConversionCompatibility<TextureCubemapArray, TextureCubemap>(
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionCompatibility compatibility;
        compatibility.IsDirectlyCompatible = true;
        compatibility.IsLossyConversion = true;
        compatibility.ConversionCost = 0.2f;
        compatibility.ConversionPath = "TextureCubemapArray -> TextureCubemap (extract first element)";
        return compatibility;
    }

    // ConvertResource specializations
    template<>
    ConversionResult<UniformBufferArray> ResourceConverter::ConvertResource<UniformBuffer, UniformBufferArray>(
        const Ref<UniformBuffer>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration,
        bool allowLossyConversion) const
    {
        auto startTime = std::chrono::high_resolution_clock::now();
        ConversionResult<UniformBufferArray> result;
        
        try
        {
            result.CompatibilityInfo = CheckConversionCompatibility<UniformBuffer, UniformBufferArray>(nullptr, targetDeclaration);
            result.ConvertedResource = ConvertToUniformBufferArray(source, targetDeclaration);
            
            if (result.ConvertedResource)
            {
                result.ResultStatus = ConversionResult<UniformBufferArray>::Status::Success;
            }
            else
            {
                result.ResultStatus = ConversionResult<UniformBufferArray>::Status::Failed;
                result.ErrorMessage = "Failed to create UniformBufferArray from UniformBuffer";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<UniformBufferArray>::Status::Failed;
            result.ErrorMessage = std::string("Exception during conversion: ") + e.what();
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        result.ActualConversionTime = std::chrono::duration<f32, std::milli>(endTime - startTime).count();
        
        UpdateStatistics("UniformBuffer->UniformBufferArray", result.IsSuccessful(), result.ActualConversionTime);
        
        return result;
    }

    // Implementation of built-in conversion functions
    ConversionResult<UniformBufferArray> ResourceConverter::ConvertToUniformBufferArray(
        const Ref<UniformBuffer>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<UniformBufferArray> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<UniformBufferArray>::Status::Failed;
            result.ErrorMessage = "Source UniformBuffer is null";
            return result;
        }

        try
        {
            // Determine array size from declaration or use default
            u32 arraySize = 1;
            if (targetDeclaration && targetDeclaration->IsArray)
            {
                arraySize = std::max(1u, targetDeclaration->ArraySize);
            }

            // Create the array with the source buffer as the first element
            auto bufferArray = CreateRef<UniformBufferArray>("ConvertedArray", 0, arraySize);
            if (bufferArray->SetBuffer(0, source))
            {
                result.ConvertedResource = bufferArray;
                result.ResultStatus = ConversionResult<UniformBufferArray>::Status::Success;
            }
            else
            {
                result.ResultStatus = ConversionResult<UniformBufferArray>::Status::Failed;
                result.ErrorMessage = "Failed to set buffer in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<UniformBufferArray>::Status::Failed;
            result.ErrorMessage = std::string("Exception during array creation: ") + e.what();
        }

        return result;
    }

    ConversionResult<StorageBufferArray> ResourceConverter::ConvertToStorageBufferArray(
        const Ref<StorageBuffer>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<StorageBufferArray> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<StorageBufferArray>::Status::Failed;
            result.ErrorMessage = "Source StorageBuffer is null";
            return result;
        }

        try
        {
            u32 arraySize = 1;
            if (targetDeclaration && targetDeclaration->IsArray)
            {
                arraySize = std::max(1u, targetDeclaration->ArraySize);
            }

            auto bufferArray = CreateRef<StorageBufferArray>("ConvertedArray", 0, arraySize);
            if (bufferArray->SetBuffer(0, source))
            {
                result.ConvertedResource = bufferArray;
                result.ResultStatus = ConversionResult<StorageBufferArray>::Status::Success;
            }
            else
            {
                result.ResultStatus = ConversionResult<StorageBufferArray>::Status::Failed;
                result.ErrorMessage = "Failed to set buffer in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<StorageBufferArray>::Status::Failed;
            result.ErrorMessage = std::string("Exception during array creation: ") + e.what();
        }

        return result;
    }

    ConversionResult<Texture2DArray> ResourceConverter::ConvertToTexture2DArray(
        const Ref<Texture2D>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<Texture2DArray> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<Texture2DArray>::Status::Failed;
            result.ErrorMessage = "Source Texture2D is null";
            return result;
        }

        try
        {
            u32 arraySize = 1;
            if (targetDeclaration && targetDeclaration->IsArray)
            {
                arraySize = std::max(1u, targetDeclaration->ArraySize);
            }

            auto textureArray = CreateRef<Texture2DArray>("ConvertedArray", 0, arraySize);
            if (textureArray->SetTexture(0, source))
            {
                result.ConvertedResource = textureArray;
                result.ResultStatus = ConversionResult<Texture2DArray>::Status::Success;
            }
            else
            {
                result.ResultStatus = ConversionResult<Texture2DArray>::Status::Failed;
                result.ErrorMessage = "Failed to set texture in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<Texture2DArray>::Status::Failed;
            result.ErrorMessage = std::string("Exception during array creation: ") + e.what();
        }

        return result;
    }

    ConversionResult<TextureCubemapArray> ResourceConverter::ConvertToTextureCubemapArray(
        const Ref<TextureCubemap>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<TextureCubemapArray> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<TextureCubemapArray>::Status::Failed;
            result.ErrorMessage = "Source TextureCubemap is null";
            return result;
        }

        try
        {
            u32 arraySize = 1;
            if (targetDeclaration && targetDeclaration->IsArray)
            {
                arraySize = std::max(1u, targetDeclaration->ArraySize);
            }

            auto textureArray = CreateRef<TextureCubemapArray>("ConvertedArray", 0, arraySize);
            if (textureArray->SetTexture(0, source))
            {
                result.ConvertedResource = textureArray;
                result.ResultStatus = ConversionResult<TextureCubemapArray>::Status::Success;
            }
            else
            {
                result.ResultStatus = ConversionResult<TextureCubemapArray>::Status::Failed;
                result.ErrorMessage = "Failed to set texture in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<TextureCubemapArray>::Status::Failed;
            result.ErrorMessage = std::string("Exception during array creation: ") + e.what();
        }

        return result;
    }

    // Reverse conversions (array to single element)
    ConversionResult<UniformBuffer> ResourceConverter::ConvertFromUniformBufferArray(
        const Ref<UniformBufferArray>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<UniformBuffer> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<UniformBuffer>::Status::Failed;
            result.ErrorMessage = "Source UniformBufferArray is null";
            return result;
        }

        try
        {
            // Extract the first buffer from the array
            auto buffer = source->GetBuffer(0);
            if (buffer)
            {
                result.ConvertedResource = buffer;
                result.ResultStatus = ConversionResult<UniformBuffer>::Status::Success;
                if (source->GetCurrentCount() > 1)
                {
                    result.WarningMessage = "Array contains multiple buffers; only the first was extracted";
                }
            }
            else
            {
                result.ResultStatus = ConversionResult<UniformBuffer>::Status::Failed;
                result.ErrorMessage = "No buffer found at index 0 in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<UniformBuffer>::Status::Failed;
            result.ErrorMessage = std::string("Exception during extraction: ") + e.what();
        }

        return result;
    }

    ConversionResult<StorageBuffer> ResourceConverter::ConvertFromStorageBufferArray(
        const Ref<StorageBufferArray>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<StorageBuffer> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<StorageBuffer>::Status::Failed;
            result.ErrorMessage = "Source StorageBufferArray is null";
            return result;
        }

        try
        {
            auto buffer = source->GetBuffer(0);
            if (buffer)
            {
                result.ConvertedResource = buffer;
                result.ResultStatus = ConversionResult<StorageBuffer>::Status::Success;
                if (source->GetCurrentCount() > 1)
                {
                    result.WarningMessage = "Array contains multiple buffers; only the first was extracted";
                }
            }
            else
            {
                result.ResultStatus = ConversionResult<StorageBuffer>::Status::Failed;
                result.ErrorMessage = "No buffer found at index 0 in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<StorageBuffer>::Status::Failed;
            result.ErrorMessage = std::string("Exception during extraction: ") + e.what();
        }

        return result;
    }

    ConversionResult<Texture2D> ResourceConverter::ConvertFromTexture2DArray(
        const Ref<Texture2DArray>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<Texture2D> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<Texture2D>::Status::Failed;
            result.ErrorMessage = "Source Texture2DArray is null";
            return result;
        }

        try
        {
            auto texture = source->GetTexture(0);
            if (texture)
            {
                result.ConvertedResource = texture;
                result.ResultStatus = ConversionResult<Texture2D>::Status::Success;
                if (source->GetCurrentCount() > 1)
                {
                    result.WarningMessage = "Array contains multiple textures; only the first was extracted";
                }
            }
            else
            {
                result.ResultStatus = ConversionResult<Texture2D>::Status::Failed;
                result.ErrorMessage = "No texture found at index 0 in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<Texture2D>::Status::Failed;
            result.ErrorMessage = std::string("Exception during extraction: ") + e.what();
        }

        return result;
    }

    ConversionResult<TextureCubemap> ResourceConverter::ConvertFromTextureCubemapArray(
        const Ref<TextureCubemapArray>& source,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        ConversionResult<TextureCubemap> result;
        
        if (!source)
        {
            result.ResultStatus = ConversionResult<TextureCubemap>::Status::Failed;
            result.ErrorMessage = "Source TextureCubemapArray is null";
            return result;
        }

        try
        {
            auto texture = source->GetTexture(0);
            if (texture)
            {
                result.ConvertedResource = texture;
                result.ResultStatus = ConversionResult<TextureCubemap>::Status::Success;
                if (source->GetCurrentCount() > 1)
                {
                    result.WarningMessage = "Array contains multiple textures; only the first was extracted";
                }
            }
            else
            {
                result.ResultStatus = ConversionResult<TextureCubemap>::Status::Failed;
                result.ErrorMessage = "No texture found at index 0 in array";
            }
        }
        catch (const std::exception& e)
        {
            result.ResultStatus = ConversionResult<TextureCubemap>::Status::Failed;
            result.ErrorMessage = std::string("Exception during extraction: ") + e.what();
        }

        return result;
    }

    // Utility functions
    std::string ResourceConverter::GetConversionKey(std::type_index fromType, std::type_index toType) const
    {
        std::ostringstream oss;
        oss << fromType.name() << "->" << toType.name();
        return oss.str();
    }

    std::string ResourceConverter::GetConversionKey(ShaderResourceType fromType, ShaderResourceType toType) const
    {
        std::ostringstream oss;
        oss << static_cast<int>(fromType) << "->" << static_cast<int>(toType);
        return oss.str();
    }

    void ResourceConverter::InitializeDefaultConverters()
    {
        InitializeBuiltinConversions();
        
        OLO_CORE_INFO("ResourceConverter: Initialized with default converters");
    }

    void ResourceConverter::InitializeBuiltinConversions()
    {
        // Register forward conversions (single to array)
        RegisterCompatibility(ShaderResourceType::UniformBuffer, ShaderResourceType::UniformBufferArray,
            { true, false, false, true, false, 0.3f, "UniformBuffer -> UniformBufferArray" });
        
        RegisterCompatibility(ShaderResourceType::StorageBuffer, ShaderResourceType::StorageBufferArray,
            { true, false, false, true, false, 0.3f, "StorageBuffer -> StorageBufferArray" });
        
        RegisterCompatibility(ShaderResourceType::Texture2D, ShaderResourceType::Texture2DArray,
            { true, false, false, true, false, 0.4f, "Texture2D -> Texture2DArray" });
        
        RegisterCompatibility(ShaderResourceType::TextureCube, ShaderResourceType::TextureCubeArray,
            { true, false, false, true, false, 0.5f, "TextureCube -> TextureCubeArray" });

        // Register reverse conversions (array to single)
        RegisterCompatibility(ShaderResourceType::UniformBufferArray, ShaderResourceType::UniformBuffer,
            { true, false, false, false, true, 0.1f, "UniformBufferArray -> UniformBuffer (extract first)" });
        
        RegisterCompatibility(ShaderResourceType::StorageBufferArray, ShaderResourceType::StorageBuffer,
            { true, false, false, false, true, 0.1f, "StorageBufferArray -> StorageBuffer (extract first)" });
        
        RegisterCompatibility(ShaderResourceType::Texture2DArray, ShaderResourceType::Texture2D,
            { true, false, false, false, true, 0.2f, "Texture2DArray -> Texture2D (extract first)" });
        
        RegisterCompatibility(ShaderResourceType::TextureCubeArray, ShaderResourceType::TextureCube,
            { true, false, false, false, true, 0.2f, "TextureCubeArray -> TextureCube (extract first)" });
    }

    void ResourceConverter::RegisterCompatibility(ShaderResourceType fromType, ShaderResourceType toType, 
                                                const ConversionCompatibility& compatibility)
    {
        std::string key = GetConversionKey(fromType, toType);
        m_CompatibilityMatrix[key] = compatibility;
    }

    void ResourceConverter::UpdateStatistics(const std::string& conversionKey, bool success, f32 conversionTime) const
    {
        m_Statistics.TotalConversions++;
        if (success)
            m_Statistics.SuccessfulConversions++;
        else
            m_Statistics.FailedConversions++;
        
        m_Statistics.TotalConversionTime += conversionTime;
        m_Statistics.AverageConversionTime = m_Statistics.TotalConversionTime / m_Statistics.TotalConversions;
        
        m_Statistics.ConversionCounts[conversionKey]++;
    }

    f32 ResourceConverter::EstimateConversionCost(ShaderResourceType fromType, ShaderResourceType toType,
                                                size_t sourceSize, size_t targetSize) const
    {
        std::string key = GetConversionKey(fromType, toType);
        auto it = m_CompatibilityMatrix.find(key);
        if (it != m_CompatibilityMatrix.end())
        {
            f32 baseCost = it->second.ConversionCost;
            
            // Adjust cost based on size if provided
            if (sourceSize > 0 && targetSize > 0)
            {
                f32 sizeRatio = static_cast<f32>(targetSize) / static_cast<f32>(sourceSize);
                baseCost *= (1.0f + (sizeRatio - 1.0f) * 0.1f); // 10% cost increase per size ratio unit
            }
            
            return std::min(baseCost, 1.0f);
        }
        
        return 1.0f; // Maximum cost for unknown conversions
    }

    bool ResourceConverter::IsConversionAvailable(ShaderResourceType fromType, ShaderResourceType toType) const
    {
        if (fromType == toType)
            return true;
        
        std::string key = GetConversionKey(fromType, toType);
        return m_CompatibilityMatrix.find(key) != m_CompatibilityMatrix.end();
    }

    std::vector<ShaderResourceType> ResourceConverter::GetSupportedConversions(ShaderResourceType sourceType) const
    {
        std::vector<ShaderResourceType> conversions;
        
        // Check all possible target types
        for (int i = 0; i < static_cast<int>(ShaderResourceType::PushConstant) + 1; ++i)
        {
            ShaderResourceType targetType = static_cast<ShaderResourceType>(i);
            if (IsConversionAvailable(sourceType, targetType))
            {
                conversions.push_back(targetType);
            }
        }
        
        return conversions;
    }

    std::vector<ShaderResourceType> ResourceConverter::GetConversionPath(
        ShaderResourceType fromType, ShaderResourceType toType,
        const OpenGLResourceDeclaration::ResourceInfo* sourceDeclaration,
        const OpenGLResourceDeclaration::ResourceInfo* targetDeclaration) const
    {
        // For now, only direct conversions are supported
        // Future enhancement could implement multi-step conversion paths
        if (IsConversionAvailable(fromType, toType))
        {
            return { fromType, toType };
        }
        
        return {}; // No path available
    }

    // Global instance
    ResourceConverter& GetResourceConverter()
    {
        static ResourceConverter instance;
        static bool initialized = false;
        
        if (!initialized)
        {
            instance.InitializeDefaultConverters();
            initialized = true;
        }
        
        return instance;
    }
}
