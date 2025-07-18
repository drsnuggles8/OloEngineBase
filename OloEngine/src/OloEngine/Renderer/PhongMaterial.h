#pragma once

#include "OloEngine/Renderer/IMaterial.h"
#include "OloEngine/Renderer/Texture.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Phong Material implementation
     * 
     * Implements traditional Phong shading material for backward compatibility
     * and simpler rendering scenarios.
     */
    class PhongMaterial : public IMaterial
    {
    public:
        PhongMaterial();
        explicit PhongMaterial(const std::string& name);
        ~PhongMaterial() override = default;

        // IMaterial interface
        MaterialType GetType() const override { return MaterialType::Phong; }
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

        // Phong-specific properties
        glm::vec3 Ambient = glm::vec3(0.1f);
        glm::vec3 Diffuse = glm::vec3(0.8f);
        glm::vec3 Specular = glm::vec3(1.0f);
        f32 Shininess = 32.0f;
        bool UseTextureMaps = false;

        // Texture maps
        Ref<Texture2D> DiffuseMap;
        Ref<Texture2D> SpecularMap;

        /**
         * @brief Set ambient color
         */
        void SetAmbient(const glm::vec3& ambient) { Ambient = ambient; }

        /**
         * @brief Set diffuse color
         */
        void SetDiffuse(const glm::vec3& diffuse) { Diffuse = diffuse; }

        /**
         * @brief Set specular color
         */
        void SetSpecular(const glm::vec3& specular) { Specular = specular; }

        /**
         * @brief Set shininess factor
         */
        void SetShininess(float shininess) { Shininess = shininess; }

        /**
         * @brief Check if texture maps are available
         */
        bool HasDiffuseMap() const { return DiffuseMap != nullptr; }
        bool HasSpecularMap() const { return SpecularMap != nullptr; }

    private:
        std::string m_Name = "PhongMaterial";
        Ref<Shader> m_Shader;

        /**
         * @brief Update material uniform buffer with current values
         */
        void UpdateMaterialUBO();
    };
}
