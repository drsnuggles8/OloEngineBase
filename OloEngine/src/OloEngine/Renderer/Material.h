#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Containers/Map.h"
#include "OloEngine/Renderer/RendererResource.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

namespace OloEngine
{

    enum class MaterialFlag : u32
    {
        None = 0,
        DepthTest = 1u << 0,
        Blend = 1u << 1,
        TwoSided = 1u << 2,
        DisableShadowCasting = 1u << 3
    };

    enum class MaterialType
    {
        Legacy = 0, // Legacy Phong-style material
        PBR = 1     // Physically Based Rendering material
    };

    // @brief Material class for handling PBR and legacy material properties
    //
    // This class uses a consistent encapsulated design with getter/setter methods.
    // All material properties are accessed through typed methods that handle
    // both the uniform system and direct property access efficiently.
    //
    // Features:
    // - Unified interface for both PBR and legacy materials
    // - Automatic uniform management for shader binding
    // - Type-safe property access with validation
    // - Efficient texture and parameter caching
    // - Asset dependency tracking integration
    class Material : public RendererResource
    {
      public:
        // Default constructor for struct-like usage
        Material();

        // Copy constructor for value semantics (Model.cpp uses Material as value type)
        Material(const Material& other);

        // Assignment operator for value semantics
        Material& operator=(const Material& other);

        // Move constructor for efficient transfers
        Material(Material&& other) = default;

        // Move assignment operator for efficient transfers
        Material& operator=(Material&& other) = default;

        static Ref<Material> Create(const Ref<OloEngine::Shader>& shader, const std::string& name = "");
        static Ref<Material> Copy(const Ref<Material>& other, const std::string& name = "");

        // Static factory method for PBR materials - returns Ref<Material> for consistency
        static Ref<Material> CreatePBR(const std::string& name, const glm::vec3& baseColor, float metallic = 0.0f, float roughness = 0.5f);
        // Static factory for snow PBR material (white, high roughness, non-metallic)
        static Ref<Material> CreateSnow(const std::string& name = "Snow");

        virtual ~Material() = default;

        virtual void Invalidate() {}
        virtual void OnShaderReloaded() {}

        // Material property accessors
        void SetName(const std::string& name)
        {
            m_Name = name;
        }
        const std::string& GetName() const
        {
            return m_Name;
        }

        void SetType(MaterialType type)
        {
            m_MaterialType = type;
        }
        MaterialType GetType() const
        {
            return m_MaterialType;
        }

        void SetShader(const Ref<Shader>& shader)
        {
            m_Shader = shader;
        }
        const Ref<OloEngine::Shader>& GetShader() const
        {
            return m_Shader;
        }

        virtual void Set(const std::string& name, float value);
        virtual void Set(const std::string& name, int value);
        virtual void Set(const std::string& name, u32 value);
        virtual void Set(const std::string& name, bool value);
        virtual void Set(const std::string& name, const glm::vec2& value);
        virtual void Set(const std::string& name, const glm::vec3& value);
        virtual void Set(const std::string& name, const glm::vec4& value);
        virtual void Set(const std::string& name, const glm::ivec2& value);
        virtual void Set(const std::string& name, const glm::ivec3& value);
        virtual void Set(const std::string& name, const glm::ivec4& value);

        virtual void Set(const std::string& name, const glm::mat3& value);
        virtual void Set(const std::string& name, const glm::mat4& value);

        virtual void Set(const std::string& name, const Ref<Texture2D>& texture);
        virtual void Set(const std::string& name, const Ref<Texture2D>& texture, u32 arrayIndex);
        virtual void Set(const std::string& name, const Ref<TextureCubemap>& texture);

        virtual float GetFloat(const std::string& name) const;
        virtual i32 GetInt(const std::string& name) const;
        virtual u32 GetUInt(const std::string& name) const;
        virtual bool GetBool(const std::string& name) const;
        virtual const glm::vec2& GetVector2(const std::string& name) const;
        virtual const glm::vec3& GetVector3(const std::string& name) const;
        virtual const glm::vec4& GetVector4(const std::string& name) const;
        virtual const glm::ivec2& GetIntVector2(const std::string& name) const;
        virtual const glm::ivec3& GetIntVector3(const std::string& name) const;
        virtual const glm::ivec4& GetIntVector4(const std::string& name) const;
        virtual const glm::mat3& GetMatrix3(const std::string& name) const;
        virtual const glm::mat4& GetMatrix4(const std::string& name) const;

        virtual Ref<Texture2D> GetTexture2D(const std::string& name);
        virtual Ref<Texture2D> GetTexture2D(const std::string& name, u32 arrayIndex);
        virtual Ref<TextureCubemap> GetTextureCube(const std::string& name);

        // Const overloads that forward to the non-const virtuals for backward compatibility
        Ref<Texture2D> GetTexture2D(const std::string& name) const;
        Ref<Texture2D> GetTexture2D(const std::string& name, u32 arrayIndex) const;
        Ref<TextureCubemap> GetTextureCube(const std::string& name) const;

        [[nodiscard]] virtual Ref<Texture2D> TryGetTexture2D(const std::string& name);
        [[nodiscard]] virtual Ref<Texture2D> TryGetTexture2D(const std::string& name, u32 arrayIndex);
        virtual Ref<TextureCubemap> TryGetTextureCube(const std::string& name);

        // Const overloads that forward to the non-const virtuals for backward compatibility
        [[nodiscard]] Ref<Texture2D> TryGetTexture2D(const std::string& name) const;
        [[nodiscard]] Ref<Texture2D> TryGetTexture2D(const std::string& name, u32 arrayIndex) const;
        Ref<TextureCubemap> TryGetTextureCube(const std::string& name) const;

        virtual u32 GetFlags() const
        {
            return m_MaterialFlags;
        }
        virtual void SetFlags(u32 flags)
        {
            m_MaterialFlags = flags;
        }

        virtual bool GetFlag(MaterialFlag flag) const
        {
            return (static_cast<u32>(flag) & m_MaterialFlags) != 0;
        }
        virtual void SetFlag(MaterialFlag flag, bool value = true);

        // IBL configuration method (for Sandbox3D compatibility)
        void ConfigureIBL(const Ref<TextureCubemap>& environmentMap,
                          const Ref<TextureCubemap>& irradianceMap,
                          const Ref<TextureCubemap>& prefilterMap,
                          const Ref<Texture2D>& brdfLutMap);

        // =====================================================================
        // TYPED PROPERTY ACCESSORS (Replacement for public member variables)
        // =====================================================================

        // Legacy material properties (for backward compatibility)
        const glm::vec3& GetAmbient() const
        {
            return m_Ambient;
        }
        void SetAmbient(const glm::vec3& ambient)
        {
            m_Ambient = ambient;
        }
        const glm::vec3& GetDiffuse() const
        {
            return m_Diffuse;
        }
        void SetDiffuse(const glm::vec3& diffuse)
        {
            m_Diffuse = diffuse;
        }
        const glm::vec3& GetSpecular() const
        {
            return m_Specular;
        }
        void SetSpecular(const glm::vec3& specular)
        {
            m_Specular = specular;
        }
        float GetShininess() const
        {
            return m_Shininess;
        }
        void SetShininess(float shininess)
        {
            m_Shininess = shininess;
        }
        bool IsUsingTextureMaps() const
        {
            return m_UseTextureMaps;
        }
        void SetUseTextureMaps(bool use)
        {
            m_UseTextureMaps = use;
        }
        Ref<Texture2D> GetDiffuseMap() const
        {
            return m_DiffuseMap;
        }
        void SetDiffuseMap(const Ref<Texture2D>& texture)
        {
            m_DiffuseMap = texture;
        }
        Ref<Texture2D> GetSpecularMap() const
        {
            return m_SpecularMap;
        }
        void SetSpecularMap(const Ref<Texture2D>& texture)
        {
            m_SpecularMap = texture;
        }

        // PBR material properties
        const glm::vec4& GetBaseColorFactor() const
        {
            return m_BaseColorFactor;
        }
        void SetBaseColorFactor(const glm::vec4& color)
        {
            m_BaseColorFactor = color;
        }
        const glm::vec4& GetEmissiveFactor() const
        {
            return m_EmissiveFactor;
        }
        void SetEmissiveFactor(const glm::vec4& emissive)
        {
            m_EmissiveFactor = emissive;
        }
        float GetMetallicFactor() const
        {
            return m_MetallicFactor;
        }
        void SetMetallicFactor(float metallic)
        {
            m_MetallicFactor = metallic;
        }
        float GetRoughnessFactor() const
        {
            return m_RoughnessFactor;
        }
        void SetRoughnessFactor(float roughness)
        {
            m_RoughnessFactor = roughness;
        }
        float GetNormalScale() const
        {
            return m_NormalScale;
        }
        void SetNormalScale(float scale)
        {
            m_NormalScale = scale;
        }
        float GetOcclusionStrength() const
        {
            return m_OcclusionStrength;
        }
        void SetOcclusionStrength(float strength)
        {
            m_OcclusionStrength = strength;
        }
        bool IsIBLEnabled() const
        {
            return m_EnableIBL;
        }
        void SetEnableIBL(bool enable)
        {
            m_EnableIBL = enable;
        }

        // PBR texture maps
        Ref<Texture2D> GetAlbedoMap() const
        {
            return m_AlbedoMap;
        }
        void SetAlbedoMap(const Ref<Texture2D>& texture)
        {
            m_AlbedoMap = texture;
        }
        Ref<Texture2D> GetMetallicRoughnessMap() const
        {
            return m_MetallicRoughnessMap;
        }
        void SetMetallicRoughnessMap(const Ref<Texture2D>& texture)
        {
            m_MetallicRoughnessMap = texture;
        }
        Ref<Texture2D> GetNormalMap() const
        {
            return m_NormalMap;
        }
        void SetNormalMap(const Ref<Texture2D>& texture)
        {
            m_NormalMap = texture;
        }
        Ref<Texture2D> GetAOMap() const
        {
            return m_AOMap;
        }
        void SetAOMap(const Ref<Texture2D>& texture)
        {
            m_AOMap = texture;
        }
        Ref<Texture2D> GetEmissiveMap() const
        {
            return m_EmissiveMap;
        }
        void SetEmissiveMap(const Ref<Texture2D>& texture)
        {
            m_EmissiveMap = texture;
        }
        Ref<TextureCubemap> GetEnvironmentMap() const
        {
            return m_EnvironmentMap;
        }
        void SetEnvironmentMap(const Ref<TextureCubemap>& texture)
        {
            m_EnvironmentMap = texture;
        }
        Ref<TextureCubemap> GetIrradianceMap() const
        {
            return m_IrradianceMap;
        }
        void SetIrradianceMap(const Ref<TextureCubemap>& texture)
        {
            m_IrradianceMap = texture;
        }
        Ref<TextureCubemap> GetPrefilterMap() const
        {
            return m_PrefilterMap;
        }
        void SetPrefilterMap(const Ref<TextureCubemap>& texture)
        {
            m_PrefilterMap = texture;
        }
        Ref<Texture2D> GetBRDFLutMap() const
        {
            return m_BRDFLutMap;
        }
        void SetBRDFLutMap(const Ref<Texture2D>& texture)
        {
            m_BRDFLutMap = texture;
        }

        // =====================================================================
        // Asset interface
        static AssetType GetStaticType()
        {
            return AssetType::Material;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }

        // Accessors for serialization
        const TMap<std::string, float>& GetFloatUniforms() const
        {
            return m_FloatUniforms;
        }
        const TMap<std::string, int>& GetIntUniforms() const
        {
            return m_IntUniforms;
        }
        const TMap<std::string, u32>& GetUIntUniforms() const
        {
            return m_UIntUniforms;
        }
        const TMap<std::string, bool>& GetBoolUniforms() const
        {
            return m_BoolUniforms;
        }
        const TMap<std::string, glm::vec2>& GetVec2Uniforms() const
        {
            return m_Vec2Uniforms;
        }
        const TMap<std::string, glm::vec3>& GetVec3Uniforms() const
        {
            return m_Vec3Uniforms;
        }
        const TMap<std::string, glm::vec4>& GetVec4Uniforms() const
        {
            return m_Vec4Uniforms;
        }
        const TMap<std::string, glm::ivec2>& GetIVec2Uniforms() const
        {
            return m_IVec2Uniforms;
        }
        const TMap<std::string, glm::ivec3>& GetIVec3Uniforms() const
        {
            return m_IVec3Uniforms;
        }
        const TMap<std::string, glm::ivec4>& GetIVec4Uniforms() const
        {
            return m_IVec4Uniforms;
        }
        const TMap<std::string, glm::mat3>& GetMat3Uniforms() const
        {
            return m_Mat3Uniforms;
        }
        const TMap<std::string, glm::mat4>& GetMat4Uniforms() const
        {
            return m_Mat4Uniforms;
        }
        const TMap<std::string, Ref<Texture2D>>& GetTexture2DUniforms() const
        {
            return m_Texture2DUniforms;
        }
        const TMap<std::string, Ref<TextureCubemap>>& GetTextureCubeUniforms() const
        {
            return m_TextureCubeUniforms;
        }

      protected:
        Material(const Ref<OloEngine::Shader>& shader, const std::string& name = "");

        // Helper method to generate composite keys for array textures
        static std::string GenerateArrayKey(const std::string& name, u32 arrayIndex);

      protected:
        Ref<OloEngine::Shader> m_Shader;
        std::string m_Name;
        u32 m_MaterialFlags = static_cast<u32>(MaterialFlag::DepthTest);

        // Material properties storage (uniform system)
        // Using TMap for better cache performance on hot path (every draw call)
        TMap<std::string, float> m_FloatUniforms;
        TMap<std::string, int> m_IntUniforms;
        TMap<std::string, u32> m_UIntUniforms;
        TMap<std::string, bool> m_BoolUniforms;
        TMap<std::string, glm::vec2> m_Vec2Uniforms;
        TMap<std::string, glm::vec3> m_Vec3Uniforms;
        TMap<std::string, glm::vec4> m_Vec4Uniforms;
        TMap<std::string, glm::ivec2> m_IVec2Uniforms;
        TMap<std::string, glm::ivec3> m_IVec3Uniforms;
        TMap<std::string, glm::ivec4> m_IVec4Uniforms;
        TMap<std::string, glm::mat3> m_Mat3Uniforms;
        TMap<std::string, glm::mat4> m_Mat4Uniforms;
        TMap<std::string, Ref<Texture2D>> m_Texture2DUniforms;
        TMap<std::string, Ref<TextureCubemap>> m_TextureCubeUniforms;

        // =====================================================================
        // PRIVATE MATERIAL PROPERTIES (Encapsulated)
        // =====================================================================

        // Material type
        MaterialType m_MaterialType = MaterialType::PBR;

        // Legacy material properties (for backward compatibility)
        glm::vec3 m_Ambient = glm::vec3(0.2f);
        glm::vec3 m_Diffuse = glm::vec3(0.8f);
        glm::vec3 m_Specular = glm::vec3(1.0f);
        float m_Shininess = 32.0f;
        bool m_UseTextureMaps = false;
        Ref<Texture2D> m_DiffuseMap;
        Ref<Texture2D> m_SpecularMap;

        // PBR material properties
        glm::vec4 m_BaseColorFactor = glm::vec4(1.0f); // Base color (albedo) with alpha
        glm::vec4 m_EmissiveFactor = glm::vec4(0.0f);  // Emissive color
        float m_MetallicFactor = 0.0f;                 // Metallic factor
        float m_RoughnessFactor = 1.0f;                // Roughness factor
        float m_NormalScale = 1.0f;                    // Normal map scale
        float m_OcclusionStrength = 1.0f;              // AO strength
        bool m_EnableIBL = false;                      // Enable IBL

        // PBR texture maps
        Ref<Texture2D> m_AlbedoMap;            // Base color texture
        Ref<Texture2D> m_MetallicRoughnessMap; // Metallic-roughness texture (glTF format)
        Ref<Texture2D> m_NormalMap;            // Normal map
        Ref<Texture2D> m_AOMap;                // Ambient occlusion map
        Ref<Texture2D> m_EmissiveMap;          // Emissive map
        Ref<TextureCubemap> m_EnvironmentMap;  // Environment cubemap
        Ref<TextureCubemap> m_IrradianceMap;   // Irradiance cubemap
        Ref<TextureCubemap> m_PrefilterMap;    // Prefiltered environment map
        Ref<Texture2D> m_BRDFLutMap;           // BRDF lookup table
    };

} // namespace OloEngine
