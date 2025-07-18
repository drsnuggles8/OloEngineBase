#pragma once

#include "OloEngine/Renderer/IMaterial.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include <glm/glm.hpp>

// Forward declaration to avoid circular dependency
namespace OloEngine { enum class PBRValidationLevel; struct PBRValidationResult; }

namespace OloEngine
{
    /**
     * @brief PBR Material implementation
     * 
     * Implements physically based rendering material following the
     * glTF 2.0 metallic-roughness workflow.
     */
    class PBRMaterial : public IMaterial
    {
    public:
        PBRMaterial();
        explicit PBRMaterial(const std::string& name);
        ~PBRMaterial() override = default;

        // IMaterial interface
        MaterialType GetType() const override { return MaterialType::PBR; }
        const std::string& GetName() const override { return m_Name; }
        void SetName(const std::string& name) override { m_Name = name; }
        Ref<Shader> GetShader() const override { return m_Shader; }
        void SetShader(const Ref<Shader>& shader) override;
        bool Validate() const override;
        void ApplyToShader() override;
        ShaderResourceRegistry* GetResourceRegistry() override;
        const ShaderResourceRegistry* GetResourceRegistry() const override;
        u64 CalculateKey() const override;
        bool operator==(const IMaterial& other) const override;

        // PBR-specific properties
        glm::vec4 BaseColorFactor = glm::vec4(1.0f);     // Base color (albedo) with alpha
        glm::vec4 EmissiveFactor = glm::vec4(0.0f);      // Emissive color
        f32 MetallicFactor = 0.0f;                       // Metallic factor
        f32 RoughnessFactor = 1.0f;                      // Roughness factor
        f32 NormalScale = 1.0f;                          // Normal map scale
        f32 OcclusionStrength = 1.0f;                    // AO strength
        bool EnableIBL = false;                          // Enable IBL

        // PBR texture maps
        Ref<Texture2D> AlbedoMap;                        // Base color texture
        Ref<Texture2D> MetallicRoughnessMap;             // Metallic-roughness texture (glTF format)
        Ref<Texture2D> NormalMap;                        // Normal map
        Ref<Texture2D> AOMap;                            // Ambient occlusion map
        Ref<Texture2D> EmissiveMap;                      // Emissive map
        Ref<TextureCubemap> EnvironmentMap;              // Environment cubemap
        Ref<TextureCubemap> IrradianceMap;               // Irradiance cubemap
        Ref<TextureCubemap> PrefilterMap;                // Prefiltered environment map
        Ref<Texture2D> BRDFLutMap;                       // BRDF lookup table

        /**
         * @brief Configure PBR textures for this material
         * Automatically sets up PBR texture bindings based on available texture maps
         */
        void ConfigurePBRTextures();

        /**
         * @brief Set base color (albedo)
         */
        void SetBaseColor(const glm::vec3& color) { BaseColorFactor = glm::vec4(color, BaseColorFactor.a); }
        void SetBaseColor(const glm::vec4& color) { BaseColorFactor = color; }

        /**
         * @brief Set metallic and roughness factors
         */
        void SetMetallicRoughness(float metallic, float roughness) 
        { 
            MetallicFactor = metallic; 
            RoughnessFactor = roughness; 
        }

        /**
         * @brief Set emissive color
         */
        void SetEmissive(const glm::vec3& emissive) { EmissiveFactor = glm::vec4(emissive, 0.0f); }

        /**
         * @brief Check if texture maps are available
         */
        bool HasAlbedoMap() const { return AlbedoMap != nullptr; }
        bool HasMetallicRoughnessMap() const { return MetallicRoughnessMap != nullptr; }
        bool HasNormalMap() const { return NormalMap != nullptr; }
        bool HasAOMap() const { return AOMap != nullptr; }
        bool HasEmissiveMap() const { return EmissiveMap != nullptr; }
        bool HasIBLMaps() const { return IrradianceMap != nullptr && PrefilterMap != nullptr && BRDFLutMap != nullptr; }
        
        // Enhanced validation and error handling
        PBRValidationResult ValidateEnhanced(PBRValidationLevel level = static_cast<PBRValidationLevel>(1)) const; // Standard level
        void RecoverFromErrors();
        
        // Performance monitoring
        void EnablePerformanceMonitoring(bool enable) { m_PerformanceMonitoring = enable; }
        bool IsPerformanceMonitoringEnabled() const { return m_PerformanceMonitoring; }

        /**
         * @brief Convert PBRMaterial to legacy Material struct for backward compatibility
         * @return Material struct with equivalent properties
         */
        Material ToMaterial() const;

        /**
         * @brief Intelligently select appropriate shader based on lighting conditions
         * @param lightCount Number of active lights in the scene
         * @param isSkinnedMesh Whether this material is being used for a skinned mesh
         * @return Best shader for current conditions
         */
        static Ref<Shader> SelectOptimalShader(int lightCount, bool isSkinnedMesh = false);

    private:
        std::string m_Name = "PBRMaterial";
        Ref<Shader> m_Shader;
        bool m_PerformanceMonitoring = false;

        /**
         * @brief Update material uniform buffer with current values
         */
        void UpdateMaterialUBO();
    };
}
