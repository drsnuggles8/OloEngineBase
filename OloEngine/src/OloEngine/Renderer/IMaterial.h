#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    enum class MaterialType
    {
        Unknown = 0,
        Phong,
        PBR
    };

    /**
     * @brief Interface for polymorphic material handling
     * 
     * This interface provides a common base for all material types,
     * allowing for type-safe material management and proper validation.
     */
    class IMaterial
    {
    public:
        virtual ~IMaterial() = default;

        /**
         * @brief Get the material type
         */
        virtual MaterialType GetType() const = 0;

        /**
         * @brief Get the material name
         */
        virtual const std::string& GetName() const = 0;

        /**
         * @brief Set the material name
         */
        virtual void SetName(const std::string& name) = 0;

        /**
         * @brief Get the associated shader
         */
        virtual Ref<Shader> GetShader() const = 0;

        /**
         * @brief Set the associated shader
         */
        virtual void SetShader(const Ref<Shader>& shader) = 0;

        /**
         * @brief Validate the material configuration
         * @return true if material is valid and ready for rendering
         */
        virtual bool Validate() const = 0;

        /**
         * @brief Apply the material to its shader
         * This should be called before rendering with this material
         */
        virtual void ApplyToShader() = 0;

        /**
         * @brief Get the resource registry for this material
         */
        virtual ShaderResourceRegistry* GetResourceRegistry() = 0;
        virtual const ShaderResourceRegistry* GetResourceRegistry() const = 0;

        /**
         * @brief Calculate a unique hash key for this material
         * Used for material batching and caching
         */
        virtual u64 CalculateKey() const = 0;

        /**
         * @brief Check if two materials are equal
         */
        virtual bool operator==(const IMaterial& other) const = 0;

    protected:
        /**
         * @brief Helper function to hash combine values
         */
        template <typename T>
        static void HashCombine(u64& seed, const T& v)
        {
            std::hash<T> hasher;
            seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
    };

    /**
     * @brief Material factory for creating typed materials
     */
    class MaterialFactory
    {
    public:
        /**
         * @brief Create a new material of the specified type
         */
        static Ref<IMaterial> Create(MaterialType type);

        /**
         * @brief Create a PBR material with basic parameters
         */
        static Ref<IMaterial> CreatePBRMaterial(
            const glm::vec3& baseColor,
            float metallic = 0.0f,
            float roughness = 0.5f
        );

        /**
         * @brief Create a Phong material with basic parameters
         */
        static Ref<IMaterial> CreatePhongMaterial(
            const glm::vec3& ambient,
            const glm::vec3& diffuse,
            const glm::vec3& specular,
            float shininess = 32.0f
        );
    };
}
