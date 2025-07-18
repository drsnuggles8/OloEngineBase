#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PBRMaterial.h"
#include "OloEngine/Renderer/PBRValidation.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{
    PBRMaterial::PBRMaterial()
    {
        // Set default PBR values
        BaseColorFactor = glm::vec4(1.0f);
        EmissiveFactor = glm::vec4(0.0f);
        MetallicFactor = 0.0f;
        RoughnessFactor = 1.0f;
        NormalScale = 1.0f;
        OcclusionStrength = 1.0f;
        EnableIBL = false;
    }

    PBRMaterial::PBRMaterial(const std::string& name)
        : PBRMaterial()
    {
        m_Name = name;
    }

    void PBRMaterial::SetShader(const Ref<Shader>& shader)
    {
        if (!shader)
        {
            OLO_CORE_ERROR("PBRMaterial::SetShader: Cannot set null shader");
            return;
        }

        m_Shader = shader;
        
        // Validate that this is a PBR-compatible shader
        if (!Validate())
        {
            OLO_CORE_WARN("PBRMaterial::SetShader: Shader may not be compatible with PBR rendering");
        }
    }

    bool PBRMaterial::Validate() const
    {
        if (!m_Shader)
        {
            OLO_CORE_ERROR("PBRMaterial::Validate: No shader associated with material '{}'", m_Name);
            return false;
        }

        // Validate PBR property ranges
        if (MetallicFactor < 0.0f || MetallicFactor > 1.0f)
        {
            OLO_CORE_ERROR("PBRMaterial::Validate: MetallicFactor out of range [0,1]: {} for material '{}'", MetallicFactor, m_Name);
            return false;
        }
        
        if (RoughnessFactor < 0.0f || RoughnessFactor > 1.0f)
        {
            OLO_CORE_ERROR("PBRMaterial::Validate: RoughnessFactor out of range [0,1]: {} for material '{}'", RoughnessFactor, m_Name);
            return false;
        }
        
        if (NormalScale < 0.0f)
        {
            OLO_CORE_ERROR("PBRMaterial::Validate: NormalScale cannot be negative: {} for material '{}'", NormalScale, m_Name);
            return false;
        }
        
        if (OcclusionStrength < 0.0f || OcclusionStrength > 1.0f)
        {
            OLO_CORE_ERROR("PBRMaterial::Validate: OcclusionStrength out of range [0,1]: {} for material '{}'", OcclusionStrength, m_Name);
            return false;
        }

        // Validate IBL setup if enabled
        if (EnableIBL)
        {
            if (!IrradianceMap || !PrefilterMap || !BRDFLutMap)
            {
                OLO_CORE_ERROR("PBRMaterial::Validate: IBL enabled but missing required IBL textures for material '{}'", m_Name);
                return false;
            }
        }

        // Check if shader has required PBR uniforms
        auto* registry = GetResourceRegistry();
        if (!registry)
        {
            OLO_CORE_ERROR("PBRMaterial::Validate: Cannot access shader resource registry for material '{}'", m_Name);
            return false;
        }

        return true;
    }

    void PBRMaterial::ApplyToShader()
    {
        if (!m_Shader)
        {
            OLO_CORE_ERROR("PBRMaterial::ApplyToShader: No shader associated with material '{}'", m_Name);
            return;
        }

        // Performance monitoring
        if (m_PerformanceMonitoring)
        {
            PBRPerformanceMonitor::RecordMaterialSwitch();
        }

        // Update material UBO
        UpdateMaterialUBO();

        // Configure PBR textures
        ConfigurePBRTextures();

        // Apply all resource bindings
        auto* registry = GetResourceRegistry();
        if (registry)
        {
            registry->ApplyBindings();
        }
    }

    ShaderResourceRegistry* PBRMaterial::GetResourceRegistry()
    {
        return m_Shader ? m_Shader->GetResourceRegistry() : nullptr;
    }

    const ShaderResourceRegistry* PBRMaterial::GetResourceRegistry() const
    {
        return m_Shader ? m_Shader->GetResourceRegistry() : nullptr;
    }

    void PBRMaterial::ConfigurePBRTextures()
    {
        auto* registry = GetResourceRegistry();
        if (!registry)
        {
            OLO_CORE_ERROR("PBRMaterial::ConfigurePBRTextures: No resource registry available for material '{}'", m_Name);
            return;
        }

        // Bind PBR textures using standard binding layout
        if (AlbedoMap)
            registry->SetTexture("u_AlbedoMap", AlbedoMap);
        if (MetallicRoughnessMap)
            registry->SetTexture("u_MetallicRoughnessMap", MetallicRoughnessMap);
        if (NormalMap)
            registry->SetTexture("u_NormalMap", NormalMap);
        if (AOMap)
            registry->SetTexture("u_AOMap", AOMap);
        if (EmissiveMap)
            registry->SetTexture("u_EmissiveMap", EmissiveMap);
        if (EnvironmentMap)
            registry->SetTexture("u_EnvironmentMap", EnvironmentMap);
        if (IrradianceMap)
            registry->SetTexture("u_IrradianceMap", IrradianceMap);
        if (PrefilterMap)
            registry->SetTexture("u_PrefilterMap", PrefilterMap);
        if (BRDFLutMap)
            registry->SetTexture("u_BRDFLutMap", BRDFLutMap);
    }

    void PBRMaterial::UpdateMaterialUBO()
    {
        auto* registry = GetResourceRegistry();
        if (!registry)
            return;

        // Create PBR material data
        ShaderBindingLayout::PBRMaterialUBO materialData;
        materialData.BaseColorFactor = BaseColorFactor;
        materialData.EmissiveFactor = EmissiveFactor;
        materialData.MetallicFactor = MetallicFactor;
        materialData.RoughnessFactor = RoughnessFactor;
        materialData.NormalScale = NormalScale;
        materialData.OcclusionStrength = OcclusionStrength;
        materialData.UseAlbedoMap = HasAlbedoMap() ? 1 : 0;
        materialData.UseNormalMap = HasNormalMap() ? 1 : 0;
        materialData.UseMetallicRoughnessMap = HasMetallicRoughnessMap() ? 1 : 0;
        materialData.UseAOMap = HasAOMap() ? 1 : 0;
        materialData.UseEmissiveMap = HasEmissiveMap() ? 1 : 0;
        materialData.EnableIBL = EnableIBL ? 1 : 0;

        // Set UBO data through registry
        // Note: This would need a UBO resource to be available in the registry
        // For now, we'll use the traditional uniform approach
        if (m_Shader)
        {
            m_Shader->Bind();
            m_Shader->SetFloat4("u_BaseColorFactor", BaseColorFactor);
            m_Shader->SetFloat4("u_EmissiveFactor", EmissiveFactor);
            m_Shader->SetFloat("u_MetallicFactor", MetallicFactor);
            m_Shader->SetFloat("u_RoughnessFactor", RoughnessFactor);
            m_Shader->SetFloat("u_NormalScale", NormalScale);
            m_Shader->SetFloat("u_OcclusionStrength", OcclusionStrength);
            m_Shader->SetInt("u_UseAlbedoMap", materialData.UseAlbedoMap);
            m_Shader->SetInt("u_UseNormalMap", materialData.UseNormalMap);
            m_Shader->SetInt("u_UseMetallicRoughnessMap", materialData.UseMetallicRoughnessMap);
            m_Shader->SetInt("u_UseAOMap", materialData.UseAOMap);
            m_Shader->SetInt("u_UseEmissiveMap", materialData.UseEmissiveMap);
            m_Shader->SetInt("u_EnableIBL", materialData.EnableIBL);
        }
    }

    u64 PBRMaterial::CalculateKey() const
    {
        u64 key = 0;
        
        // Include PBR properties
        HashCombine(key, std::hash<glm::vec4>()(BaseColorFactor));
        HashCombine(key, std::hash<glm::vec4>()(EmissiveFactor));
        HashCombine(key, std::hash<float>()(MetallicFactor));
        HashCombine(key, std::hash<float>()(RoughnessFactor));
        HashCombine(key, std::hash<float>()(NormalScale));
        HashCombine(key, std::hash<float>()(OcclusionStrength));
        HashCombine(key, std::hash<bool>()(EnableIBL));
        
        // Include shader ID if available
        if (m_Shader)
            HashCombine(key, m_Shader->GetRendererID());
        
        // Include PBR texture IDs
        if (AlbedoMap)
            HashCombine(key, AlbedoMap->GetRendererID());
        if (MetallicRoughnessMap)
            HashCombine(key, MetallicRoughnessMap->GetRendererID());
        if (NormalMap)
            HashCombine(key, NormalMap->GetRendererID());
        if (AOMap)
            HashCombine(key, AOMap->GetRendererID());
        if (EmissiveMap)
            HashCombine(key, EmissiveMap->GetRendererID());
        if (EnvironmentMap)
            HashCombine(key, EnvironmentMap->GetRendererID());
        if (IrradianceMap)
            HashCombine(key, IrradianceMap->GetRendererID());
        if (PrefilterMap)
            HashCombine(key, PrefilterMap->GetRendererID());
        if (BRDFLutMap)
            HashCombine(key, BRDFLutMap->GetRendererID());
        
        return key;
    }

    bool PBRMaterial::operator==(const IMaterial& other) const
    {
        if (other.GetType() != MaterialType::PBR)
            return false;

        const auto& pbrOther = static_cast<const PBRMaterial&>(other);
        
        // Compare PBR properties
        if (BaseColorFactor != pbrOther.BaseColorFactor ||
            EmissiveFactor != pbrOther.EmissiveFactor ||
            MetallicFactor != pbrOther.MetallicFactor ||
            RoughnessFactor != pbrOther.RoughnessFactor ||
            NormalScale != pbrOther.NormalScale ||
            OcclusionStrength != pbrOther.OcclusionStrength ||
            EnableIBL != pbrOther.EnableIBL)
        {
            return false;
        }

        // Compare texture maps
        if ((AlbedoMap && pbrOther.AlbedoMap && *AlbedoMap != *pbrOther.AlbedoMap) ||
            (AlbedoMap && !pbrOther.AlbedoMap) || (!AlbedoMap && pbrOther.AlbedoMap))
            return false;
            
        if ((MetallicRoughnessMap && pbrOther.MetallicRoughnessMap && *MetallicRoughnessMap != *pbrOther.MetallicRoughnessMap) ||
            (MetallicRoughnessMap && !pbrOther.MetallicRoughnessMap) || (!MetallicRoughnessMap && pbrOther.MetallicRoughnessMap))
            return false;
            
        if ((NormalMap && pbrOther.NormalMap && *NormalMap != *pbrOther.NormalMap) ||
            (NormalMap && !pbrOther.NormalMap) || (!NormalMap && pbrOther.NormalMap))
            return false;
            
        if ((AOMap && pbrOther.AOMap && *AOMap != *pbrOther.AOMap) ||
            (AOMap && !pbrOther.AOMap) || (!AOMap && pbrOther.AOMap))
            return false;
            
        if ((EmissiveMap && pbrOther.EmissiveMap && *EmissiveMap != *pbrOther.EmissiveMap) ||
            (EmissiveMap && !pbrOther.EmissiveMap) || (!EmissiveMap && pbrOther.EmissiveMap))
            return false;

        return true;
    }

    // Enhanced validation and error handling implementation
    PBRValidationResult PBRMaterial::ValidateEnhanced(PBRValidationLevel level) const
    {
        return PBRValidator::ValidateMaterial(*this, level);
    }

    void PBRMaterial::RecoverFromErrors()
    {
        OLO_CORE_WARN("Attempting to recover PBR material: {}", m_Name);
        
        // Use the error recovery system to fix common issues
        PBRErrorRecovery::RecoverMaterial(*this);
        
        // Re-validate after recovery
        auto result = ValidateEnhanced(PBRValidationLevel::Basic);
        if (result.IsValid)
        {
            OLO_CORE_INFO("PBR material recovery successful for: {}", m_Name);
        }
        else
        {
            OLO_CORE_ERROR("PBR material recovery failed for: {}", m_Name);
            result.LogSummary();
        }
    }

    Material PBRMaterial::ToMaterial() const
    {
        Material material;
        
        // Copy PBR properties
        material.BaseColorFactor = BaseColorFactor;
        material.EmissiveFactor = EmissiveFactor;
        material.MetallicFactor = MetallicFactor;
        material.RoughnessFactor = RoughnessFactor;
        material.NormalScale = NormalScale;
        material.OcclusionStrength = OcclusionStrength;
        material.EnablePBR = true;
        material.EnableIBL = EnableIBL;
        
        // Copy texture maps
        material.AlbedoMap = AlbedoMap;
        material.MetallicRoughnessMap = MetallicRoughnessMap;
        material.NormalMap = NormalMap;
        material.AOMap = AOMap;
        material.EmissiveMap = EmissiveMap;
        material.EnvironmentMap = EnvironmentMap;
        material.IrradianceMap = IrradianceMap;
        material.PrefilterMap = PrefilterMap;
        material.BRDFLutMap = BRDFLutMap;
        
        // Set shader
        material.Shader = m_Shader;
        
        // Set legacy properties for backward compatibility
        glm::vec3 baseColor = glm::vec3(BaseColorFactor);
        material.Ambient = baseColor * 0.1f;
        material.Diffuse = baseColor;
        material.Specular = glm::mix(glm::vec3(0.04f), baseColor, MetallicFactor);
        material.Shininess = (1.0f - RoughnessFactor) * 128.0f;
        material.UseTextureMaps = HasAlbedoMap() || HasNormalMap() || HasMetallicRoughnessMap();
        
        return material;
    }

    Ref<Shader> PBRMaterial::SelectOptimalShader(int lightCount, bool isSkinnedMesh)
    {
        // Performance monitoring
        PBRPerformanceMonitor::RecordShaderSwitch();
        
        // Shader selection logic based on lighting conditions
        if (lightCount <= 1)
        {
            // Single light or no lights - use basic PBR shader
            if (isSkinnedMesh)
            {
                // Use PBR_Skinned shader for single light with skinning
                return Renderer3D::GetShaderLibrary().Get("PBR_Skinned");
            }
            else
            {
                // Use basic PBR shader for single light without skinning
                return Renderer3D::GetShaderLibrary().Get("PBR");
            }
        }
        else
        {
            // Multiple lights - use multi-light PBR shader
            if (isSkinnedMesh)
            {
                // Use PBR_MultiLight_Skinned shader for multiple lights with skinning
                return Renderer3D::GetShaderLibrary().Get("PBR_MultiLight_Skinned");
            }
            else
            {
                // Use PBR_MultiLight shader for multiple lights without skinning
                return Renderer3D::GetShaderLibrary().Get("PBR_MultiLight");
            }
        }
    }
}
