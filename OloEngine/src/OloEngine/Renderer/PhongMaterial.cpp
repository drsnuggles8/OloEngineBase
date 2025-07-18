#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PhongMaterial.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "Platform/OpenGL/OpenGLShader.h"

namespace OloEngine
{
    PhongMaterial::PhongMaterial()
    {
        // Set default Phong values
        Ambient = glm::vec3(0.1f);
        Diffuse = glm::vec3(0.8f);
        Specular = glm::vec3(1.0f);
        Shininess = 32.0f;
        UseTextureMaps = false;
    }

    PhongMaterial::PhongMaterial(const std::string& name)
        : PhongMaterial()
    {
        m_Name = name;
    }

    void PhongMaterial::SetShader(const Ref<Shader>& shader)
    {
        if (!shader)
        {
            OLO_CORE_ERROR("PhongMaterial::SetShader: Cannot set null shader");
            return;
        }

        m_Shader = shader;
        
        // Validate that this is a Phong-compatible shader
        if (!Validate())
        {
            OLO_CORE_WARN("PhongMaterial::SetShader: Shader may not be compatible with Phong rendering");
        }
    }

    bool PhongMaterial::Validate() const
    {
        if (!m_Shader)
        {
            OLO_CORE_ERROR("PhongMaterial::Validate: No shader associated with material '{}'", m_Name);
            return false;
        }

        // Check if shader has required Phong uniforms
        auto* registry = GetResourceRegistry();
        if (!registry)
        {
            OLO_CORE_ERROR("PhongMaterial::Validate: Cannot access shader resource registry for material '{}'", m_Name);
            return false;
        }

        // For Phong, we're more lenient as it's simpler
        return true;
    }

    void PhongMaterial::ApplyToShader()
    {
        if (!m_Shader)
        {
            OLO_CORE_ERROR("PhongMaterial::ApplyToShader: No shader associated with material '{}'", m_Name);
            return;
        }

        // Update material UBO
        UpdateMaterialUBO();

        // Apply all resource bindings
        auto* registry = GetResourceRegistry();
        if (registry)
        {
            // Bind texture maps
            if (DiffuseMap)
                registry->SetTexture("u_DiffuseMap", DiffuseMap);
            if (SpecularMap)
                registry->SetTexture("u_SpecularMap", SpecularMap);

            registry->ApplyBindings();
        }
    }

    ShaderResourceRegistry* PhongMaterial::GetResourceRegistry()
    {
        if (!m_Shader)
            return nullptr;

        if (auto* openglShader = dynamic_cast<OpenGLShader*>(m_Shader.get()))
        {
            return &openglShader->GetResourceRegistry();
        }
        
        return nullptr;
    }

    const ShaderResourceRegistry* PhongMaterial::GetResourceRegistry() const
    {
        if (!m_Shader)
            return nullptr;

        if (auto* openglShader = dynamic_cast<const OpenGLShader*>(m_Shader.get()))
        {
            return &openglShader->GetResourceRegistry();
        }
        
        return nullptr;
    }

    void PhongMaterial::UpdateMaterialUBO()
    {
        if (!m_Shader)
            return;

        // Use traditional uniform approach for Phong materials
        m_Shader->Bind();
        m_Shader->SetFloat3("u_MaterialAmbient", Ambient);
        m_Shader->SetFloat3("u_MaterialDiffuse", Diffuse);
        m_Shader->SetFloat3("u_MaterialSpecular", Specular);
        m_Shader->SetFloat("u_MaterialShininess", Shininess);
        m_Shader->SetInt("u_UseTextureMaps", UseTextureMaps ? 1 : 0);
    }

    u64 PhongMaterial::CalculateKey() const
    {
        u64 key = 0;
        
        // Include Phong properties
        HashCombine(key, std::hash<glm::vec3>()(Ambient));
        HashCombine(key, std::hash<glm::vec3>()(Diffuse));
        HashCombine(key, std::hash<glm::vec3>()(Specular));
        HashCombine(key, std::hash<float>()(Shininess));
        HashCombine(key, std::hash<bool>()(UseTextureMaps));
        
        // Include shader ID if available
        if (m_Shader)
            HashCombine(key, m_Shader->GetRendererID());
        
        // Include texture IDs if used
        if (UseTextureMaps)
        {
            if (DiffuseMap)
                HashCombine(key, DiffuseMap->GetRendererID());
            if (SpecularMap)
                HashCombine(key, SpecularMap->GetRendererID());
        }
        
        return key;
    }

    bool PhongMaterial::operator==(const IMaterial& other) const
    {
        if (other.GetType() != MaterialType::Phong)
            return false;

        const auto& phongOther = static_cast<const PhongMaterial&>(other);
        
        // Compare Phong properties
        if (Ambient != phongOther.Ambient ||
            Diffuse != phongOther.Diffuse ||
            Specular != phongOther.Specular ||
            Shininess != phongOther.Shininess ||
            UseTextureMaps != phongOther.UseTextureMaps)
        {
            return false;
        }

        // Compare texture maps if they are used
        if (UseTextureMaps)
        {
            if (DiffuseMap && phongOther.DiffuseMap)
            {
                if (*DiffuseMap != *phongOther.DiffuseMap)
                    return false;
            }
            else if (DiffuseMap || phongOther.DiffuseMap)
            {
                return false;
            }

            if (SpecularMap && phongOther.SpecularMap)
            {
                if (*SpecularMap != *phongOther.SpecularMap)
                    return false;
            }
            else if (SpecularMap || phongOther.SpecularMap)
            {
                return false;
            }
        }

        return true;
    }
}
