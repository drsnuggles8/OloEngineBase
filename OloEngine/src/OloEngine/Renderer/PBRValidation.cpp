// =============================================================================
// PBRValidation.cpp - Implementation of PBR validation and error handling
// Part of OloEngine Enhanced PBR System
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PBRValidation.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Renderer.h"

#include <glm/glm.hpp>
#include <chrono>

namespace OloEngine
{
    // Static member definitions for fallback textures
    Ref<Texture2D> PBRErrorRecovery::s_FallbackAlbedo = nullptr;
    Ref<Texture2D> PBRErrorRecovery::s_FallbackNormal = nullptr;
    Ref<Texture2D> PBRErrorRecovery::s_FallbackMetallicRoughness = nullptr;
    Ref<Texture2D> PBRErrorRecovery::s_FallbackAO = nullptr;
    Ref<TextureCubemap> PBRErrorRecovery::s_FallbackEnvironment = nullptr;

    // Performance monitor static members
    PBRPerformanceMonitor::RenderStats PBRPerformanceMonitor::s_CurrentFrameStats;
    std::chrono::high_resolution_clock::time_point PBRPerformanceMonitor::s_FrameStartTime;

    // =============================================================================
    // PBRValidator Implementation
    // =============================================================================

    PBRValidationResult PBRValidator::ValidateMaterial(const Material& material, PBRValidationLevel level)
    {
        OLO_PROFILE_FUNCTION();
        
        PBRValidationResult result;
        
        // Basic validation
        if (material.GetName().empty())
        {
            result.AddWarning("Material has no name");
        }
        
        // Validate PBR values
        auto valueResult = ValidatePBRValues(material.MetallicFactor, material.RoughnessFactor, 
                                           material.BaseColorFactor, level);
        result.Warnings.insert(result.Warnings.end(), valueResult.Warnings.begin(), valueResult.Warnings.end());
        result.Errors.insert(result.Errors.end(), valueResult.Errors.begin(), valueResult.Errors.end());
        if (!valueResult.IsValid) result.IsValid = false;
        
        // Validate textures
        if (material.AlbedoMap)
        {
            auto textureResult = ValidateTexture(material.AlbedoMap, "AlbedoMap", level);
            result.Warnings.insert(result.Warnings.end(), textureResult.Warnings.begin(), textureResult.Warnings.end());
            result.Errors.insert(result.Errors.end(), textureResult.Errors.begin(), textureResult.Errors.end());
            if (!textureResult.IsValid) result.IsValid = false;
        }
        
        if (material.NormalMap)
        {
            auto textureResult = ValidateTexture(material.NormalMap, "NormalMap", level);
            result.Warnings.insert(result.Warnings.end(), textureResult.Warnings.begin(), textureResult.Warnings.end());
            result.Errors.insert(result.Errors.end(), textureResult.Errors.begin(), textureResult.Errors.end());
            if (!textureResult.IsValid) result.IsValid = false;
        }
        
        if (material.MetallicRoughnessMap)
        {
            auto textureResult = ValidateTexture(material.MetallicRoughnessMap, "MetallicRoughnessMap", level);
            result.Warnings.insert(result.Warnings.end(), textureResult.Warnings.begin(), textureResult.Warnings.end());
            result.Errors.insert(result.Errors.end(), textureResult.Errors.begin(), textureResult.Errors.end());
            if (!textureResult.IsValid) result.IsValid = false;
        }
        
        // Validate IBL setup if enabled
        if (material.EnableIBL)
        {
            auto iblResult = ValidateIBLSetup(material.IrradianceMap, material.PrefilterMap, 
                                            material.BRDFLutMap, level);
            result.Warnings.insert(result.Warnings.end(), iblResult.Warnings.begin(), iblResult.Warnings.end());
            result.Errors.insert(result.Errors.end(), iblResult.Errors.begin(), iblResult.Errors.end());
            if (!iblResult.IsValid) result.IsValid = false;
        }
        
        // Standard and above validation
        if (level >= PBRValidationLevel::Standard)
        {
            // Check for shader compatibility
            if (material.GetShader())
            {
                auto shaderResult = ValidateShader(material.GetShader(), level);
                result.Warnings.insert(result.Warnings.end(), shaderResult.Warnings.begin(), shaderResult.Warnings.end());
                result.Errors.insert(result.Errors.end(), shaderResult.Errors.begin(), shaderResult.Errors.end());
                if (!shaderResult.IsValid) result.IsValid = false;
            }
            else
            {
                result.AddError("Material has no shader assigned");
            }
        }
        
        // Strict and debug validation
        if (level >= PBRValidationLevel::Strict)
        {
            // Performance analysis
            AnalyzePerformance(material);
            
            // Check texture consistency
            if (material.AlbedoMap && material.NormalMap)
            {
                if (material.AlbedoMap->GetWidth() != material.NormalMap->GetWidth() ||
                    material.AlbedoMap->GetHeight() != material.NormalMap->GetHeight())
                {
                    result.AddWarning("Albedo and normal maps have different resolutions");
                }
            }
        }
        
        return result;
    }

    PBRValidationResult PBRValidator::ValidateTexture(const Ref<Texture2D>& texture, 
                                                     const std::string& textureName,
                                                     PBRValidationLevel level)
    {
        PBRValidationResult result;
        
        if (!texture)
        {
            result.AddError("Texture '" + textureName + "' is null");
            return result;
        }
        
        // Basic validation
        if (texture->GetWidth() == 0 || texture->GetHeight() == 0)
        {
            result.AddError("Texture '" + textureName + "' has invalid dimensions");
        }
        
        // Validate texture size
        if (!IsValidTextureSize(texture->GetWidth(), texture->GetHeight(), textureName))
        {
            result.AddWarning("Texture '" + textureName + "' has non-power-of-two dimensions");
        }
        
        // Standard validation
        if (level >= PBRValidationLevel::Standard)
        {
            // Check filtering and memory usage
            CheckTextureFiltering(texture, textureName, result);
            CheckMemoryUsage(texture, textureName, result);
            
            // Validate format for specific texture types
            if (!IsValidTextureFormat(texture->GetSpecification().Format, textureName))
            {
                result.AddWarning("Texture '" + textureName + "' may have suboptimal format");
            }
        }
        
        return result;
    }

    PBRValidationResult PBRValidator::ValidateCubemap(const Ref<TextureCubemap>& cubemap, 
                                                     const std::string& cubemapName,
                                                     PBRValidationLevel level)
    {
        PBRValidationResult result;
        
        if (!cubemap)
        {
            result.AddError("Cubemap '" + cubemapName + "' is null");
            return result;
        }
        
        // Basic validation
        if (cubemap->GetWidth() == 0 || cubemap->GetHeight() == 0)
        {
            result.AddError("Cubemap '" + cubemapName + "' has invalid dimensions");
        }
        
        if (cubemap->GetWidth() != cubemap->GetHeight())
        {
            result.AddError("Cubemap '" + cubemapName + "' is not square");
        }
        
        // Check if power of two
        if (!IsPowerOfTwo(cubemap->GetWidth()))
        {
            result.AddWarning("Cubemap '" + cubemapName + "' width is not power of two");
        }
        
        return result;
    }

    PBRValidationResult PBRValidator::ValidateShader(const Ref<Shader>& shader, PBRValidationLevel level)
    {
        PBRValidationResult result;
        
        if (!shader)
        {
            result.AddError("Shader is null");
            return result;
        }
        
        const std::string& name = shader->GetName();
        
        // Basic validation
        if (name.empty())
        {
            result.AddWarning("Shader has no name");
        }
        
        // Check for PBR compatibility
        if (name.find("PBR") == std::string::npos && name.find("pbr") == std::string::npos)
        {
            result.AddWarning("Shader name '" + name + "' doesn't indicate PBR compatibility");
        }
        
        return result;
    }

    PBRValidationResult PBRValidator::ValidateIBLSetup(const Ref<TextureCubemap>& irradianceMap,
                                                       const Ref<TextureCubemap>& prefilterMap,
                                                       const Ref<Texture2D>& brdfLutMap,
                                                       PBRValidationLevel level)
    {
        PBRValidationResult result;
        
        // Check for required IBL textures
        if (!irradianceMap)
        {
            result.AddError("IBL enabled but irradiance map is missing");
        }
        else
        {
            auto irrResult = ValidateCubemap(irradianceMap, "IrradianceMap", level);
            result.Warnings.insert(result.Warnings.end(), irrResult.Warnings.begin(), irrResult.Warnings.end());
            result.Errors.insert(result.Errors.end(), irrResult.Errors.begin(), irrResult.Errors.end());
            if (!irrResult.IsValid) result.IsValid = false;
        }
        
        if (!prefilterMap)
        {
            result.AddError("IBL enabled but prefilter map is missing");
        }
        else
        {
            auto preResult = ValidateCubemap(prefilterMap, "PrefilterMap", level);
            result.Warnings.insert(result.Warnings.end(), preResult.Warnings.begin(), preResult.Warnings.end());
            result.Errors.insert(result.Errors.end(), preResult.Errors.begin(), preResult.Errors.end());
            if (!preResult.IsValid) result.IsValid = false;
        }
        
        if (!brdfLutMap)
        {
            result.AddError("IBL enabled but BRDF LUT is missing");
        }
        else
        {
            auto brdfResult = ValidateTexture(brdfLutMap, "BRDFLutMap", level);
            result.Warnings.insert(result.Warnings.end(), brdfResult.Warnings.begin(), brdfResult.Warnings.end());
            result.Errors.insert(result.Errors.end(), brdfResult.Errors.begin(), brdfResult.Errors.end());
            if (!brdfResult.IsValid) result.IsValid = false;
        }
        
        return result;
    }

    PBRValidationResult PBRValidator::ValidatePBRValues(float metallic, float roughness, 
                                                        const glm::vec3& baseColor,
                                                        PBRValidationLevel level)
    {
        PBRValidationResult result;
        
        // Validate metallic factor (should be 0.0 or 1.0 for pure materials, in-between for blends)
        if (metallic < 0.0f || metallic > 1.0f)
        {
            result.AddError("Metallic factor " + std::to_string(metallic) + " is outside valid range [0.0, 1.0]");
        }
        
        // Validate roughness factor
        if (roughness < 0.0f || roughness > 1.0f)
        {
            result.AddError("Roughness factor " + std::to_string(roughness) + " is outside valid range [0.0, 1.0]");
        }
        
        // Validate base color
        if (baseColor.r < 0.0f || baseColor.g < 0.0f || baseColor.b < 0.0f)
        {
            result.AddError("Base color has negative components");
        }
        
        if (baseColor.r > 1.0f || baseColor.g > 1.0f || baseColor.b > 1.0f)
        {
            result.AddWarning("Base color has components > 1.0 (HDR values)");
        }
        
        // Advanced validation
        if (level >= PBRValidationLevel::Standard)
        {
            // Check for common material issues
            if (metallic > 0.9f && (baseColor.r < 0.5f || baseColor.g < 0.5f || baseColor.b < 0.5f))
            {
                result.AddWarning("High metallic value with dark base color may not be physically accurate");
            }
            
            if (roughness < 0.01f)
            {
                result.AddWarning("Very low roughness may cause rendering artifacts");
            }
        }
        
        return result;
    }

    void PBRValidator::AnalyzePerformance(const Material& material)
    {
        auto stats = PBRPerformanceMonitor::AnalyzeMaterial(material);
        
        if (stats.HasLargeTextures)
        {
            OLO_CORE_WARN("Material '{}' has large textures (max: {}x{}), consider optimization", 
                         material.GetName(), stats.MaxTextureSize, stats.MaxTextureSize);
        }
        
        if (stats.TotalMemoryUsage > (512 * 1024 * 1024)) // 512MB
        {
            OLO_CORE_WARN("Material '{}' uses {:.2f}MB of texture memory", 
                         material.GetName(), stats.TotalMemoryUsage / (1024.0f * 1024.0f));
        }
    }

    // =============================================================================
    // Helper Methods
    // =============================================================================

    bool PBRValidator::IsValidTextureFormat(ImageFormat format, const std::string& textureType)
    {
        // Recommend appropriate formats for different texture types
        if (textureType == "AlbedoMap")
        {
            return format == ImageFormat::RGBA8 || format == ImageFormat::RGB8;
        }
        else if (textureType == "NormalMap")
        {
            return format == ImageFormat::RGB8 || format == ImageFormat::RGBA8;
        }
        else if (textureType == "MetallicRoughnessMap")
        {
            return format == ImageFormat::RGB8 || format == ImageFormat::RGBA8;
        }
        
        return true; // Unknown type, assume valid
    }

    bool PBRValidator::IsValidTextureSize(u32 width, u32 height, const std::string& textureType)
    {
        // Check for power of two (recommended for mipmapping)
        return IsPowerOfTwo(width) && IsPowerOfTwo(height);
    }

    bool PBRValidator::IsPowerOfTwo(u32 value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    void PBRValidator::CheckTextureFiltering(const Ref<Texture>& texture, const std::string& textureName, 
                                           PBRValidationResult& result)
    {
        // This would check the filtering mode of the texture
        // Implementation depends on the texture API
        // For now, just a placeholder
    }

    void PBRValidator::CheckMemoryUsage(const Ref<Texture>& texture, const std::string& textureName, 
                                      PBRValidationResult& result)
    {
        // Estimate memory usage
        u32 width = 0, height = 0;
        ImageFormat format = ImageFormat::RGBA8;
        
        if (auto tex2D = std::dynamic_pointer_cast<Texture2D>(texture))
        {
            width = tex2D->GetWidth();
            height = tex2D->GetHeight();
            format = tex2D->GetSpecification().Format;
        }
        
        u32 bytesPerPixel = 4; // Default to RGBA8
        switch (format)
        {
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                break;
            case ImageFormat::RGBA32F:
                bytesPerPixel = 16;
                break;
            case ImageFormat::R8:
                bytesPerPixel = 1;
                break;
            default:
                bytesPerPixel = 4; // RGBA8
                break;
        }
        
        u64 memoryUsage = static_cast<u64>(width) * height * bytesPerPixel;
        
        if (memoryUsage > (64 * 1024 * 1024)) // 64MB
        {
            result.AddWarning("Texture '" + textureName + "' uses " + 
                            std::to_string(memoryUsage / (1024 * 1024)) + "MB of memory");
        }
    }

    // =============================================================================
    // PBRErrorRecovery Implementation
    // =============================================================================

    void PBRErrorRecovery::CreateFallbackTextures()
    {
        if (s_FallbackAlbedo) return; // Already created
        
        // Create 1x1 fallback textures with appropriate colors
        TextureSpecification spec;
        spec.Width = 1;
        spec.Height = 1;
        spec.Format = ImageFormat::RGBA8;
        
        // Fallback albedo (medium gray)
        spec.Format = ImageFormat::RGBA8;
        s_FallbackAlbedo = Texture2D::Create(spec);
        uint32_t albedoData = 0xFF808080; // Gray
        s_FallbackAlbedo->SetData(&albedoData, sizeof(albedoData));
        
        // Fallback normal (pointing up)
        s_FallbackNormal = Texture2D::Create(spec);
        uint32_t normalData = 0xFF8080FF; // (0.5, 0.5, 1.0) in RGB
        s_FallbackNormal->SetData(&normalData, sizeof(normalData));
        
        // Fallback metallic/roughness (non-metallic, medium roughness)
        s_FallbackMetallicRoughness = Texture2D::Create(spec);
        uint32_t metallicRoughnessData = 0xFF808000; // (0.0, 0.5, 0.0) - no metallic, medium roughness
        s_FallbackMetallicRoughness->SetData(&metallicRoughnessData, sizeof(metallicRoughnessData));
        
        // Fallback AO (no occlusion)
        s_FallbackAO = Texture2D::Create(spec);
        uint32_t aoData = 0xFFFFFFFF; // White (no occlusion)
        s_FallbackAO->SetData(&aoData, sizeof(aoData));
        
        OLO_CORE_INFO("PBR fallback textures created");
    }

    Ref<Texture2D> PBRErrorRecovery::GetFallbackAlbedoTexture()
    {
        CreateFallbackTextures();
        return s_FallbackAlbedo;
    }

    Ref<Texture2D> PBRErrorRecovery::GetFallbackNormalTexture()
    {
        CreateFallbackTextures();
        return s_FallbackNormal;
    }

    Ref<Texture2D> PBRErrorRecovery::GetFallbackMetallicRoughnessTexture()
    {
        CreateFallbackTextures();
        return s_FallbackMetallicRoughness;
    }

    Ref<Texture2D> PBRErrorRecovery::GetFallbackAOTexture()
    {
        CreateFallbackTextures();
        return s_FallbackAO;
    }

    Ref<TextureCubemap> PBRErrorRecovery::GetFallbackEnvironmentMap()
    {
        if (!s_FallbackEnvironment)
        {
            // Create a simple gradient environment map
            CubemapSpecification spec;
            spec.Width = 32;
            spec.Height = 32;
            spec.Format = ImageFormat::RGB32F;
            
            s_FallbackEnvironment = TextureCubemap::Create(spec);
            OLO_CORE_INFO("PBR fallback environment map created");
        }
        return s_FallbackEnvironment;
    }

    void PBRErrorRecovery::RecoverMaterial(Material& material)
    {
        OLO_CORE_WARN("Recovering PBR material: {}", material.GetName());
        
        // Replace missing textures with fallbacks
        if (!material.AlbedoMap)
        {
            material.AlbedoMap = GetFallbackAlbedoTexture();
        }
        
        if (!material.NormalMap)
        {
            material.NormalMap = GetFallbackNormalTexture();
        }
        
        if (!material.MetallicRoughnessMap)
        {
            material.MetallicRoughnessMap = GetFallbackMetallicRoughnessTexture();
        }
        
        if (!material.AOMap)
        {
            material.AOMap = GetFallbackAOTexture();
        }
        
        // Clamp values to safe ranges
        material.MetallicFactor = GetSafeMetallic(material.MetallicFactor);
        material.RoughnessFactor = GetSafeRoughness(material.RoughnessFactor);
        glm::vec3 safeBaseColor = GetSafeBaseColor(glm::vec3(material.BaseColorFactor));
        material.BaseColorFactor = glm::vec4(safeBaseColor, material.BaseColorFactor.w);
        material.NormalScale = GetSafeNormalScale(material.NormalScale);
        material.OcclusionStrength = GetSafeOcclusionStrength(material.OcclusionStrength);
    }

    glm::vec3 PBRErrorRecovery::GetSafeBaseColor(const glm::vec3& input)
    {
        return glm::clamp(input, glm::vec3(0.0f), glm::vec3(1.0f));
    }

    float PBRErrorRecovery::GetSafeMetallic(float input)
    {
        return glm::clamp(input, 0.0f, 1.0f);
    }

    float PBRErrorRecovery::GetSafeRoughness(float input)
    {
        return glm::clamp(input, 0.01f, 1.0f); // Minimum roughness to avoid artifacts
    }

    float PBRErrorRecovery::GetSafeNormalScale(float input)
    {
        return glm::clamp(input, 0.0f, 5.0f); // Reasonable range for normal scaling
    }

    float PBRErrorRecovery::GetSafeOcclusionStrength(float input)
    {
        return glm::clamp(input, 0.0f, 1.0f);
    }

    // =============================================================================
    // PBRPerformanceMonitor Implementation
    // =============================================================================

    void PBRPerformanceMonitor::BeginFrame()
    {
        s_FrameStartTime = std::chrono::high_resolution_clock::now();
        s_CurrentFrameStats = {}; // Reset stats
    }

    void PBRPerformanceMonitor::EndFrame()
    {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - s_FrameStartTime);
        s_CurrentFrameStats.FrameTime = duration.count() / 1000.0f; // Convert to milliseconds
    }

    void PBRPerformanceMonitor::RecordMaterialSwitch()
    {
        s_CurrentFrameStats.MaterialSwitches++;
    }

    void PBRPerformanceMonitor::RecordTextureBinding()
    {
        s_CurrentFrameStats.TextureBinds++;
    }

    void PBRPerformanceMonitor::RecordShaderSwitch()
    {
        s_CurrentFrameStats.ShaderSwitches++;
    }

    PBRPerformanceMonitor::MaterialStats PBRPerformanceMonitor::AnalyzeMaterial(const Material& material)
    {
        MaterialStats stats;
        
        auto analyzeTexture = [&](const Ref<Texture2D>& texture) {
            if (texture)
            {
                stats.TextureCount++;
                u32 width = texture->GetWidth();
                u32 height = texture->GetHeight();
                stats.MaxTextureSize = std::max(stats.MaxTextureSize, std::max(width, height));
                
                // Estimate memory usage (simplified)
                u64 textureMemory = static_cast<u64>(width) * height * 4; // Assume RGBA8
                stats.TotalMemoryUsage += textureMemory;
                
                if (width > 2048 || height > 2048)
                {
                    stats.HasLargeTextures = true;
                }
            }
        };
        
        analyzeTexture(material.AlbedoMap);
        analyzeTexture(material.NormalMap);
        analyzeTexture(material.MetallicRoughnessMap);
        analyzeTexture(material.AOMap);
        analyzeTexture(material.EmissiveMap);
        
        return stats;
    }

    const PBRPerformanceMonitor::RenderStats& PBRPerformanceMonitor::GetCurrentFrameStats()
    {
        return s_CurrentFrameStats;
    }

    void PBRPerformanceMonitor::LogPerformanceReport()
    {
        const auto& stats = s_CurrentFrameStats;
        OLO_CORE_INFO("PBR Performance Report - Frame Time: {:.2f}ms, Material Switches: {}, Texture Binds: {}, Shader Switches: {}",
                     stats.FrameTime, stats.MaterialSwitches, stats.TextureBinds, stats.ShaderSwitches);
    }
}
