#include "OloEnginePCH.h"
#include "Material.h"

namespace OloEngine
{

    Material::Material()
        : m_Shader(nullptr), m_Name("Material"), m_MaterialType(MaterialType::PBR)
    {
    }

    Material::Material(const Ref<OloEngine::Shader>& shader, const std::string& name)
        : m_Shader(shader), m_Name(name), m_MaterialType(MaterialType::PBR)
    {
    }

    Material::Material(const Material& other)
        : RendererResource(),
          m_Shader(other.m_Shader), m_Name(other.m_Name), m_MaterialFlags(other.m_MaterialFlags),
          m_FloatUniforms(other.m_FloatUniforms), m_IntUniforms(other.m_IntUniforms), m_UIntUniforms(other.m_UIntUniforms),
          m_BoolUniforms(other.m_BoolUniforms), m_Vec2Uniforms(other.m_Vec2Uniforms), m_Vec3Uniforms(other.m_Vec3Uniforms),
          m_Vec4Uniforms(other.m_Vec4Uniforms), m_IVec2Uniforms(other.m_IVec2Uniforms), m_IVec3Uniforms(other.m_IVec3Uniforms),
          m_IVec4Uniforms(other.m_IVec4Uniforms), m_Mat3Uniforms(other.m_Mat3Uniforms), m_Mat4Uniforms(other.m_Mat4Uniforms),
          m_Texture2DUniforms(other.m_Texture2DUniforms), m_TextureCubeUniforms(other.m_TextureCubeUniforms),
          // Copy all private members
          m_MaterialType(other.m_MaterialType),
          m_Ambient(other.m_Ambient), m_Diffuse(other.m_Diffuse), m_Specular(other.m_Specular), m_Shininess(other.m_Shininess),
          m_UseTextureMaps(other.m_UseTextureMaps), m_DiffuseMap(other.m_DiffuseMap), m_SpecularMap(other.m_SpecularMap),
          m_BaseColorFactor(other.m_BaseColorFactor), m_EmissiveFactor(other.m_EmissiveFactor),
          m_MetallicFactor(other.m_MetallicFactor), m_RoughnessFactor(other.m_RoughnessFactor),
          m_NormalScale(other.m_NormalScale), m_OcclusionStrength(other.m_OcclusionStrength), m_EnableIBL(other.m_EnableIBL),
          m_AlbedoMap(other.m_AlbedoMap), m_MetallicRoughnessMap(other.m_MetallicRoughnessMap), m_NormalMap(other.m_NormalMap),
          m_AOMap(other.m_AOMap), m_EmissiveMap(other.m_EmissiveMap), m_EnvironmentMap(other.m_EnvironmentMap),
          m_IrradianceMap(other.m_IrradianceMap), m_PrefilterMap(other.m_PrefilterMap), m_BRDFLutMap(other.m_BRDFLutMap)
    {
    }

    Material& Material::operator=(const Material& other)
    {
        if (this != &other)
        {
            m_Shader = other.m_Shader;
            m_Name = other.m_Name;
            m_MaterialFlags = other.m_MaterialFlags;
            m_FloatUniforms = other.m_FloatUniforms;
            m_IntUniforms = other.m_IntUniforms;
            m_UIntUniforms = other.m_UIntUniforms;
            m_BoolUniforms = other.m_BoolUniforms;
            m_Vec2Uniforms = other.m_Vec2Uniforms;
            m_Vec3Uniforms = other.m_Vec3Uniforms;
            m_Vec4Uniforms = other.m_Vec4Uniforms;
            m_IVec2Uniforms = other.m_IVec2Uniforms;
            m_IVec3Uniforms = other.m_IVec3Uniforms;
            m_IVec4Uniforms = other.m_IVec4Uniforms;
            m_Mat3Uniforms = other.m_Mat3Uniforms;
            m_Mat4Uniforms = other.m_Mat4Uniforms;
            m_Texture2DUniforms = other.m_Texture2DUniforms;
            m_TextureCubeUniforms = other.m_TextureCubeUniforms;

            // Copy all private members
            m_MaterialType = other.m_MaterialType;
            m_Ambient = other.m_Ambient;
            m_Diffuse = other.m_Diffuse;
            m_Specular = other.m_Specular;
            m_Shininess = other.m_Shininess;
            m_UseTextureMaps = other.m_UseTextureMaps;
            m_DiffuseMap = other.m_DiffuseMap;
            m_SpecularMap = other.m_SpecularMap;
            m_BaseColorFactor = other.m_BaseColorFactor;
            m_EmissiveFactor = other.m_EmissiveFactor;
            m_MetallicFactor = other.m_MetallicFactor;
            m_RoughnessFactor = other.m_RoughnessFactor;
            m_NormalScale = other.m_NormalScale;
            m_OcclusionStrength = other.m_OcclusionStrength;
            m_EnableIBL = other.m_EnableIBL;
            m_AlbedoMap = other.m_AlbedoMap;
            m_MetallicRoughnessMap = other.m_MetallicRoughnessMap;
            m_NormalMap = other.m_NormalMap;
            m_AOMap = other.m_AOMap;
            m_EmissiveMap = other.m_EmissiveMap;
            m_EnvironmentMap = other.m_EnvironmentMap;
            m_IrradianceMap = other.m_IrradianceMap;
            m_PrefilterMap = other.m_PrefilterMap;
            m_BRDFLutMap = other.m_BRDFLutMap;
        }
        return *this;
    }

    Ref<Material> Material::Create(const Ref<OloEngine::Shader>& shader, const std::string& name)
    {
        return Ref<Material>(new Material(shader, name));
    }

    Ref<Material> Material::Copy(const Ref<Material>& other, const std::string& name)
    {
        auto material = Ref<Material>(new Material(other->m_Shader, name.empty() ? other->m_Name : name));

        // Copy all properties
        material->m_MaterialFlags = other->m_MaterialFlags;
        material->m_FloatUniforms = other->m_FloatUniforms;
        material->m_IntUniforms = other->m_IntUniforms;
        material->m_UIntUniforms = other->m_UIntUniforms;
        material->m_BoolUniforms = other->m_BoolUniforms;
        material->m_Vec2Uniforms = other->m_Vec2Uniforms;
        material->m_Vec3Uniforms = other->m_Vec3Uniforms;
        material->m_Vec4Uniforms = other->m_Vec4Uniforms;
        material->m_Mat3Uniforms = other->m_Mat3Uniforms;
        material->m_Mat4Uniforms = other->m_Mat4Uniforms;
        material->m_Texture2DUniforms = other->m_Texture2DUniforms;
        material->m_TextureCubeUniforms = other->m_TextureCubeUniforms;

        // Copy private material properties
        material->m_AlbedoMap = other->m_AlbedoMap;
        material->m_MetallicRoughnessMap = other->m_MetallicRoughnessMap;
        material->m_NormalMap = other->m_NormalMap;
        material->m_AOMap = other->m_AOMap;
        material->m_EmissiveMap = other->m_EmissiveMap;

        return material;
    }

    Ref<Material> Material::CreatePBR(const std::string& name, const glm::vec3& baseColor, float metallic, float roughness)
    {
        auto material = Ref<Material>(new Material());
        material->m_Name = name;
        material->m_MaterialType = MaterialType::PBR;
        material->m_MaterialFlags = static_cast<u32>(MaterialFlag::DepthTest);

        // Set PBR properties using the uniform system
        material->Set("u_MaterialUniforms.AlbedoColor", baseColor);
        material->Set("u_MaterialUniforms.Metalness", metallic);
        material->Set("u_MaterialUniforms.Roughness", roughness);
        material->Set("u_MaterialUniforms.Emission", 0.0f);

        // Set the private PBR members
        material->m_BaseColorFactor = glm::vec4(baseColor, 1.0f);
        material->m_MetallicFactor = metallic;
        material->m_RoughnessFactor = roughness;
        material->m_EmissiveFactor = glm::vec4(0.0f);
        material->m_NormalScale = 1.0f;
        material->m_OcclusionStrength = 1.0f;
        material->m_EnableIBL = false;

        return material;
    }

    void Material::ConfigureIBL(const Ref<TextureCubemap>& environmentMap,
                                const Ref<TextureCubemap>& irradianceMap,
                                const Ref<TextureCubemap>& prefilterMap,
                                const Ref<Texture2D>& brdfLutMap)
    {
        m_EnvironmentMap = environmentMap;
        m_IrradianceMap = irradianceMap;
        m_PrefilterMap = prefilterMap;
        m_BRDFLutMap = brdfLutMap;
        m_EnableIBL = true;
    }

    void Material::Set(const std::string& name, float value)
    {
        m_FloatUniforms[name] = value;
    }

    void Material::Set(const std::string& name, int value)
    {
        m_IntUniforms[name] = value;
    }

    void Material::Set(const std::string& name, u32 value)
    {
        m_UIntUniforms[name] = value;
    }

    void Material::Set(const std::string& name, bool value)
    {
        m_BoolUniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::vec2& value)
    {
        m_Vec2Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::vec3& value)
    {
        m_Vec3Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::vec4& value)
    {
        m_Vec4Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::ivec2& value)
    {
        m_IVec2Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::ivec3& value)
    {
        m_IVec3Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::ivec4& value)
    {
        m_IVec4Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::mat3& value)
    {
        m_Mat3Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const glm::mat4& value)
    {
        m_Mat4Uniforms[name] = value;
    }

    void Material::Set(const std::string& name, const Ref<Texture2D>& texture)
    {
        m_Texture2DUniforms[name] = texture;
    }

    void Material::Set(const std::string& name, const Ref<Texture2D>& texture, u32 arrayIndex)
    {
        // Use composite key to support array indexing
        std::string key = GenerateArrayKey(name, arrayIndex);
        m_Texture2DUniforms[key] = texture;
    }

    void Material::Set(const std::string& name, const Ref<TextureCubemap>& texture)
    {
        m_TextureCubeUniforms[name] = texture;
    }

    float Material::GetFloat(const std::string& name) const
    {
        auto it = m_FloatUniforms.find(name);
        if (it != m_FloatUniforms.end())
            return it->second;

        // Return default value if not found
        static const float defaultValue = 0.0f;
        return defaultValue;
    }

    i32 Material::GetInt(const std::string& name) const
    {
        auto it = m_IntUniforms.find(name);
        if (it != m_IntUniforms.end())
            return it->second;

        static const i32 defaultValue = 0;
        return defaultValue;
    }

    u32 Material::GetUInt(const std::string& name) const
    {
        auto it = m_UIntUniforms.find(name);
        if (it != m_UIntUniforms.end())
            return it->second;

        static const u32 defaultValue = 0u;
        return defaultValue;
    }

    bool Material::GetBool(const std::string& name) const
    {
        auto it = m_BoolUniforms.find(name);
        if (it != m_BoolUniforms.end())
            return it->second;

        static const bool defaultValue = false;
        return defaultValue;
    }

    const glm::vec2& Material::GetVector2(const std::string& name) const
    {
        auto it = m_Vec2Uniforms.find(name);
        if (it != m_Vec2Uniforms.end())
            return it->second;

        static const glm::vec2 defaultValue = glm::vec2(0.0f);
        return defaultValue;
    }

    const glm::vec3& Material::GetVector3(const std::string& name) const
    {
        auto it = m_Vec3Uniforms.find(name);
        if (it != m_Vec3Uniforms.end())
            return it->second;

        static const glm::vec3 defaultValue = glm::vec3(0.0f);
        return defaultValue;
    }

    const glm::vec4& Material::GetVector4(const std::string& name) const
    {
        auto it = m_Vec4Uniforms.find(name);
        if (it != m_Vec4Uniforms.end())
            return it->second;

        static const glm::vec4 defaultValue = glm::vec4(0.0f);
        return defaultValue;
    }

    const glm::ivec2& Material::GetIntVector2(const std::string& name) const
    {
        auto it = m_IVec2Uniforms.find(name);
        if (it != m_IVec2Uniforms.end())
            return it->second;

        static const glm::ivec2 defaultValue = glm::ivec2(0);
        return defaultValue;
    }

    const glm::ivec3& Material::GetIntVector3(const std::string& name) const
    {
        auto it = m_IVec3Uniforms.find(name);
        if (it != m_IVec3Uniforms.end())
            return it->second;

        static const glm::ivec3 defaultValue = glm::ivec3(0);
        return defaultValue;
    }

    const glm::ivec4& Material::GetIntVector4(const std::string& name) const
    {
        auto it = m_IVec4Uniforms.find(name);
        if (it != m_IVec4Uniforms.end())
            return it->second;

        static const glm::ivec4 defaultValue = glm::ivec4(0);
        return defaultValue;
    }

    const glm::mat3& Material::GetMatrix3(const std::string& name) const
    {
        auto it = m_Mat3Uniforms.find(name);
        if (it != m_Mat3Uniforms.end())
            return it->second;

        static const glm::mat3 defaultValue = glm::mat3(1.0f);
        return defaultValue;
    }

    const glm::mat4& Material::GetMatrix4(const std::string& name) const
    {
        auto it = m_Mat4Uniforms.find(name);
        if (it != m_Mat4Uniforms.end())
            return it->second;

        static const glm::mat4 defaultValue = glm::mat4(1.0f);
        return defaultValue;
    }

    Ref<Texture2D> Material::GetTexture2D(const std::string& name)
    {
        auto it = m_Texture2DUniforms.find(name);
        if (it != m_Texture2DUniforms.end())
            return it->second;

        return nullptr;
    }

    Ref<Texture2D> Material::GetTexture2D(const std::string& name, u32 arrayIndex)
    {
        std::string key = GenerateArrayKey(name, arrayIndex);
        auto it = m_Texture2DUniforms.find(key);
        if (it != m_Texture2DUniforms.end())
            return it->second;

        return nullptr;
    }

    // Const overloads that forward to the non-const virtuals
    Ref<Texture2D> Material::GetTexture2D(const std::string& name) const
    {
        return const_cast<Material*>(this)->GetTexture2D(name);
    }

    Ref<Texture2D> Material::GetTexture2D(const std::string& name, u32 arrayIndex) const
    {
        return const_cast<Material*>(this)->GetTexture2D(name, arrayIndex);
    }

    Ref<TextureCubemap> Material::GetTextureCube(const std::string& name)
    {
        auto it = m_TextureCubeUniforms.find(name);
        if (it != m_TextureCubeUniforms.end())
            return it->second;

        return nullptr;
    }

    // Const overload that forwards to the non-const virtual
    Ref<TextureCubemap> Material::GetTextureCube(const std::string& name) const
    {
        return const_cast<Material*>(this)->GetTextureCube(name);
    }

    Ref<Texture2D> Material::TryGetTexture2D(const std::string& name)
    {
        return GetTexture2D(name);
    }

    Ref<Texture2D> Material::TryGetTexture2D(const std::string& name, u32 arrayIndex)
    {
        return GetTexture2D(name, arrayIndex);
    }

    // Const overloads that forward to the non-const virtuals for backward compatibility
    Ref<Texture2D> Material::TryGetTexture2D(const std::string& name) const
    {
        return const_cast<Material*>(this)->TryGetTexture2D(name);
    }

    Ref<Texture2D> Material::TryGetTexture2D(const std::string& name, u32 arrayIndex) const
    {
        return const_cast<Material*>(this)->TryGetTexture2D(name, arrayIndex);
    }

    Ref<TextureCubemap> Material::TryGetTextureCube(const std::string& name)
    {
        return GetTextureCube(name);
    }

    // Const overload that forwards to the non-const virtual
    Ref<TextureCubemap> Material::TryGetTextureCube(const std::string& name) const
    {
        return const_cast<Material*>(this)->TryGetTextureCube(name);
    }

    void Material::SetFlag(MaterialFlag flag, bool value)
    {
        if (value)
            m_MaterialFlags |= static_cast<u32>(flag);
        else
            m_MaterialFlags &= ~static_cast<u32>(flag);
    }

    std::string Material::GenerateArrayKey(const std::string& name, u32 arrayIndex)
    {
        return name + "[" + std::to_string(arrayIndex) + "]";
    }

} // namespace OloEngine
