#include "OloEnginePCH.h"
#include "Material.h"

namespace OloEngine {

	Material::Material(const Ref<Shader>& shader, const std::string& name)
		: m_Shader(shader), m_Name(name)
	{
	}

	Ref<Material> Material::Create(const Ref<Shader>& shader, const std::string& name)
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

	void Material::Set(const std::string& name, const Ref<TextureCube>& texture)
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

	Ref<TextureCube> Material::GetTextureCube(const std::string& name)
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

	Ref<TextureCube> Material::TryGetTextureCube(const std::string& name)
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
