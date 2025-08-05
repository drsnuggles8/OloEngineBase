#include "OloEnginePCH.h"
#include "Material.h"

// TODO: REFACTOR MATERIAL IMPLEMENTATION
// This implementation currently maintains both uniform storage AND public member variables
// which is redundant and confusing. Once Model.cpp and Renderer3D.cpp are updated to use
// proper getter/setter methods, remove all the public member handling from copy constructor,
// assignment operator, and CreatePBR method.

namespace OloEngine {

	Material::Material()
		: m_Shader(nullptr), m_Name("Material"), Type(MaterialType::PBR)
	{
	}

	Material::Material(const Ref<OloEngine::Shader>& shader, const std::string& name)
		: m_Shader(shader), m_Name(name), Type(MaterialType::PBR)
	{
	}

	Material::Material(const Material& other)
		: RefCounted(), m_Shader(other.m_Shader), m_Name(other.m_Name), m_MaterialFlags(other.m_MaterialFlags),
		  m_FloatUniforms(other.m_FloatUniforms), m_IntUniforms(other.m_IntUniforms), m_UIntUniforms(other.m_UIntUniforms),
		  m_BoolUniforms(other.m_BoolUniforms), m_Vec2Uniforms(other.m_Vec2Uniforms), m_Vec3Uniforms(other.m_Vec3Uniforms),
		  m_Vec4Uniforms(other.m_Vec4Uniforms), m_Mat3Uniforms(other.m_Mat3Uniforms), m_Mat4Uniforms(other.m_Mat4Uniforms),
		  m_Texture2DUniforms(other.m_Texture2DUniforms), m_TextureCubeUniforms(other.m_TextureCubeUniforms),
		  // Copy all public members
		  Type(other.Type), Shader(other.Shader),
		  Ambient(other.Ambient), Diffuse(other.Diffuse), Specular(other.Specular), Shininess(other.Shininess),
		  UseTextureMaps(other.UseTextureMaps), DiffuseMap(other.DiffuseMap), SpecularMap(other.SpecularMap),
		  BaseColorFactor(other.BaseColorFactor), EmissiveFactor(other.EmissiveFactor), 
		  MetallicFactor(other.MetallicFactor), RoughnessFactor(other.RoughnessFactor), 
		  NormalScale(other.NormalScale), OcclusionStrength(other.OcclusionStrength), EnableIBL(other.EnableIBL),
		  AlbedoMap(other.AlbedoMap), MetallicRoughnessMap(other.MetallicRoughnessMap), NormalMap(other.NormalMap),
		  AOMap(other.AOMap), EmissiveMap(other.EmissiveMap), EnvironmentMap(other.EnvironmentMap),
		  IrradianceMap(other.IrradianceMap), PrefilterMap(other.PrefilterMap), BRDFLutMap(other.BRDFLutMap)
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
			m_Mat3Uniforms = other.m_Mat3Uniforms;
			m_Mat4Uniforms = other.m_Mat4Uniforms;
			m_Texture2DUniforms = other.m_Texture2DUniforms;
			m_TextureCubeUniforms = other.m_TextureCubeUniforms;
			
			// Copy all public members
			Type = other.Type;
			Shader = other.Shader;
			Ambient = other.Ambient;
			Diffuse = other.Diffuse;
			Specular = other.Specular;
			Shininess = other.Shininess;
			UseTextureMaps = other.UseTextureMaps;
			DiffuseMap = other.DiffuseMap;
			SpecularMap = other.SpecularMap;
			BaseColorFactor = other.BaseColorFactor;
			EmissiveFactor = other.EmissiveFactor;
			MetallicFactor = other.MetallicFactor;
			RoughnessFactor = other.RoughnessFactor;
			NormalScale = other.NormalScale;
			OcclusionStrength = other.OcclusionStrength;
			EnableIBL = other.EnableIBL;
			AlbedoMap = other.AlbedoMap;
			MetallicRoughnessMap = other.MetallicRoughnessMap;
			NormalMap = other.NormalMap;
			AOMap = other.AOMap;
			EmissiveMap = other.EmissiveMap;
			EnvironmentMap = other.EnvironmentMap;
			IrradianceMap = other.IrradianceMap;
			PrefilterMap = other.PrefilterMap;
			BRDFLutMap = other.BRDFLutMap;
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
		
		// Copy public texture maps
		material->AlbedoMap = other->AlbedoMap;
		material->MetallicRoughnessMap = other->MetallicRoughnessMap;
		material->NormalMap = other->NormalMap;
		material->AOMap = other->AOMap;
		material->EmissiveMap = other->EmissiveMap;
		
		return material;
	}

	Material Material::CreatePBR(const std::string& name, const glm::vec3& baseColor, float metallic, float roughness)
	{
		Material material;
		material.m_Name = name;
		material.Type = MaterialType::PBR;
		material.m_MaterialFlags = static_cast<uint32_t>(MaterialFlag::DepthTest);
		
		// Set PBR properties using the uniform system
		material.Set("u_MaterialUniforms.AlbedoColor", baseColor);
		material.Set("u_MaterialUniforms.Metalness", metallic);
		material.Set("u_MaterialUniforms.Roughness", roughness);
		material.Set("u_MaterialUniforms.Emission", 0.0f);
		
		// TODO: REMOVE - TECHNICAL DEBT
		// Also set the public PBR members for direct access (compatibility with Renderer3D.cpp)
		// This should be removed once Renderer3D.cpp uses proper getter methods
		material.BaseColorFactor = glm::vec4(baseColor, 1.0f);
		material.MetallicFactor = metallic;
		material.RoughnessFactor = roughness;
		material.EmissiveFactor = glm::vec4(0.0f);
		material.NormalScale = 1.0f;
		material.OcclusionStrength = 1.0f;
		material.EnableIBL = false;
		
		return material;
	}

	void Material::Set(const std::string& name, float value)
	{
		m_FloatUniforms[name] = value;
	}

	void Material::Set(const std::string& name, int value)
	{
		m_IntUniforms[name] = value;
	}

	void Material::Set(const std::string& name, uint32_t value)
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
		// Convert to vec2 for storage
		m_Vec2Uniforms[name] = glm::vec2(value);
	}

	void Material::Set(const std::string& name, const glm::ivec3& value)
	{
		// Convert to vec3 for storage
		m_Vec3Uniforms[name] = glm::vec3(value);
	}

	void Material::Set(const std::string& name, const glm::ivec4& value)
	{
		// Convert to vec4 for storage
		m_Vec4Uniforms[name] = glm::vec4(value);
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

	void Material::Set(const std::string& name, const Ref<Texture2D>& texture, uint32_t arrayIndex)
	{
		// For now, just store the texture - array indexing can be handled later
		m_Texture2DUniforms[name] = texture;
	}

	void Material::Set(const std::string& name, const Ref<TextureCubemap>& texture)
	{
		m_TextureCubeUniforms[name] = texture;
	}

	float& Material::GetFloat(const std::string& name)
	{
		auto it = m_FloatUniforms.find(name);
		if (it != m_FloatUniforms.end())
			return it->second;
		
		// Return a reference to a new entry
		return m_FloatUniforms[name];
	}

	int32_t& Material::GetInt(const std::string& name)
	{
		auto it = m_IntUniforms.find(name);
		if (it != m_IntUniforms.end())
			return it->second;
		
		return m_IntUniforms[name];
	}

	uint32_t& Material::GetUInt(const std::string& name)
	{
		auto it = m_UIntUniforms.find(name);
		if (it != m_UIntUniforms.end())
			return it->second;
		
		return m_UIntUniforms[name];
	}

	bool& Material::GetBool(const std::string& name)
	{
		auto it = m_BoolUniforms.find(name);
		if (it != m_BoolUniforms.end())
			return it->second;
		
		return m_BoolUniforms[name];
	}

	glm::vec2& Material::GetVector2(const std::string& name)
	{
		auto it = m_Vec2Uniforms.find(name);
		if (it != m_Vec2Uniforms.end())
			return it->second;
		
		return m_Vec2Uniforms[name];
	}

	glm::vec3& Material::GetVector3(const std::string& name)
	{
		auto it = m_Vec3Uniforms.find(name);
		if (it != m_Vec3Uniforms.end())
			return it->second;
		
		return m_Vec3Uniforms[name];
	}

	glm::vec4& Material::GetVector4(const std::string& name)
	{
		auto it = m_Vec4Uniforms.find(name);
		if (it != m_Vec4Uniforms.end())
			return it->second;
		
		return m_Vec4Uniforms[name];
	}

	glm::mat3& Material::GetMatrix3(const std::string& name)
	{
		auto it = m_Mat3Uniforms.find(name);
		if (it != m_Mat3Uniforms.end())
			return it->second;
		
		return m_Mat3Uniforms[name];
	}

	glm::mat4& Material::GetMatrix4(const std::string& name)
	{
		auto it = m_Mat4Uniforms.find(name);
		if (it != m_Mat4Uniforms.end())
			return it->second;
		
		return m_Mat4Uniforms[name];
	}

	Ref<Texture2D> Material::GetTexture2D(const std::string& name)
	{
		auto it = m_Texture2DUniforms.find(name);
		if (it != m_Texture2DUniforms.end())
			return it->second;
		
		return nullptr;
	}

	Ref<TextureCubemap> Material::GetTextureCube(const std::string& name)
	{
		auto it = m_TextureCubeUniforms.find(name);
		if (it != m_TextureCubeUniforms.end())
			return it->second;
		
		return nullptr;
	}

	Ref<Texture2D> Material::TryGetTexture2D(const std::string& name)
	{
		return GetTexture2D(name);
	}

	Ref<TextureCubemap> Material::TryGetTextureCube(const std::string& name)
	{
		return GetTextureCube(name);
	}

	void Material::SetFlag(MaterialFlag flag, bool value)
	{
		if (value)
			m_MaterialFlags |= static_cast<uint32_t>(flag);
		else
			m_MaterialFlags &= ~static_cast<uint32_t>(flag);
	}

}
