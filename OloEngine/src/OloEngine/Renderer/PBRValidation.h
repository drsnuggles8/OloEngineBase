// =============================================================================
// PBRValidation.h - Comprehensive validation and error handling for PBR materials
// Part of OloEngine Enhanced PBR System
// =============================================================================

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    enum class PBRValidationLevel
    {
        Basic = 0,      // Basic validation (fast)
        Standard = 1,   // Standard validation (recommended)
        Strict = 2,     // Strict validation (thorough)
        Debug = 3       // Debug validation (comprehensive, slow)
    };

    struct PBRValidationResult
    {
        bool IsValid = true;
        std::vector<std::string> Warnings;
        std::vector<std::string> Errors;
        
        void AddWarning(const std::string& warning) 
        { 
            Warnings.push_back(warning); 
            OLO_CORE_WARN("PBR Validation Warning: {}", warning);
        }
        
        void AddError(const std::string& error) 
        { 
            Errors.push_back(error); 
            IsValid = false;
            OLO_CORE_ERROR("PBR Validation Error: {}", error);
        }
        
        void LogSummary() const
        {
            if (IsValid)
            {
                OLO_CORE_INFO("PBR Validation passed with {} warnings", Warnings.size());
            }
            else
            {
                OLO_CORE_ERROR("PBR Validation failed with {} errors and {} warnings", 
                              Errors.size(), Warnings.size());
            }
        }
    };

    class PBRValidator
    {
    public:
        // Material validation
        static PBRValidationResult ValidateMaterial(const class Material& material, 
                                                   PBRValidationLevel level = PBRValidationLevel::Standard);
        
        // Texture validation
        static PBRValidationResult ValidateTexture(const Ref<Texture2D>& texture, 
                                                  const std::string& textureName,
                                                  PBRValidationLevel level = PBRValidationLevel::Standard);
        
        static PBRValidationResult ValidateCubemap(const Ref<TextureCubemap>& cubemap, 
                                                  const std::string& cubemapName,
                                                  PBRValidationLevel level = PBRValidationLevel::Standard);
        
        // Shader validation
        static PBRValidationResult ValidateShader(const Ref<Shader>& shader, 
                                                 PBRValidationLevel level = PBRValidationLevel::Standard);
        
        // IBL validation
        static PBRValidationResult ValidateIBLSetup(const Ref<TextureCubemap>& irradianceMap,
                                                   const Ref<TextureCubemap>& prefilterMap,
                                                   const Ref<Texture2D>& brdfLutMap,
                                                   PBRValidationLevel level = PBRValidationLevel::Standard);
        
        // Value range validation
        static PBRValidationResult ValidatePBRValues(float metallic, float roughness, 
                                                    const glm::vec3& baseColor,
                                                    PBRValidationLevel level = PBRValidationLevel::Standard);
        
        // Performance analysis
        static void AnalyzePerformance(const class Material& material);
        
    private:
        // Helper methods
        static bool IsValidTextureFormat(ImageFormat format, const std::string& textureType);
        static bool IsValidTextureSize(u32 width, u32 height, const std::string& textureType);
        static bool IsPowerOfTwo(u32 value);
        static void CheckTextureFiltering(const Ref<Texture>& texture, const std::string& textureName, 
                                        PBRValidationResult& result);
        static void CheckMemoryUsage(const Ref<Texture>& texture, const std::string& textureName, 
                                    PBRValidationResult& result);
    };

    // PBR Error Recovery utility
    class PBRErrorRecovery
    {
    public:
        // Fallback textures for missing resources
        static Ref<Texture2D> GetFallbackAlbedoTexture();
        static Ref<Texture2D> GetFallbackNormalTexture();
        static Ref<Texture2D> GetFallbackMetallicRoughnessTexture();
        static Ref<Texture2D> GetFallbackAOTexture();
        static Ref<TextureCubemap> GetFallbackEnvironmentMap();
        
        // Material recovery
        static void RecoverMaterial(class Material& material);
        
        // Create safe default values
        static glm::vec3 GetSafeBaseColor(const glm::vec3& input);
        static float GetSafeMetallic(float input);
        static float GetSafeRoughness(float input);
        static float GetSafeNormalScale(float input);
        static float GetSafeOcclusionStrength(float input);
        
    private:
        // Cache fallback textures
        static Ref<Texture2D> s_FallbackAlbedo;
        static Ref<Texture2D> s_FallbackNormal;
        static Ref<Texture2D> s_FallbackMetallicRoughness;
        static Ref<Texture2D> s_FallbackAO;
        static Ref<TextureCubemap> s_FallbackEnvironment;
        
        static void CreateFallbackTextures();
    };

    // PBR Performance Monitor
    class PBRPerformanceMonitor
    {
    public:
        struct MaterialStats
        {
            u32 TextureCount = 0;
            u64 TotalMemoryUsage = 0;  // In bytes
            u32 MaxTextureSize = 0;
            bool HasLargeTextures = false;
            bool HasMipmaps = false;
            bool HasCompression = false;
        };
        
        struct RenderStats
        {
            u32 MaterialSwitches = 0;
            u32 TextureBinds = 0;
            u32 ShaderSwitches = 0;
            float FrameTime = 0.0f;
        };
        
        static void BeginFrame();
        static void EndFrame();
        static void RecordMaterialSwitch();
        static void RecordTextureBinding();
        static void RecordShaderSwitch();
        
        static MaterialStats AnalyzeMaterial(const class Material& material);
        static const RenderStats& GetCurrentFrameStats();
        static void LogPerformanceReport();
        
    private:
        static RenderStats s_CurrentFrameStats;
        static std::chrono::high_resolution_clock::time_point s_FrameStartTime;
    };

    // Utility macros for PBR validation
    #define VALIDATE_PBR_MATERIAL(material) \
        do { \
            auto result = PBRValidator::ValidateMaterial(material); \
            if (!result.IsValid) { \
                OLO_CORE_ERROR("PBR Material validation failed for: {}", material.GetName()); \
                result.LogSummary(); \
            } \
        } while(0)

    #define VALIDATE_PBR_TEXTURE(texture, name) \
        do { \
            if (texture) { \
                auto result = PBRValidator::ValidateTexture(texture, name); \
                if (!result.IsValid) { \
                    OLO_CORE_ERROR("PBR Texture validation failed for: {}", name); \
                } \
            } \
        } while(0)

    #define VALIDATE_PBR_VALUES(metallic, roughness, baseColor) \
        do { \
            auto result = PBRValidator::ValidatePBRValues(metallic, roughness, baseColor); \
            if (!result.IsValid) { \
                OLO_CORE_ERROR("PBR Values validation failed"); \
                result.LogSummary(); \
            } \
        } while(0)
}
