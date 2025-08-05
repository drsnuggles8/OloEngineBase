#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"

namespace OloEngine {

	enum class MaterialFlag
	{
		None       = 1 << 0,
		DepthTest  = 1 << 1,
		Blend      = 1 << 2,
		TwoSided   = 1 << 3,
		DisableShadowCasting = 1 << 4
	};

	class Material : public RefCounted
	{
	public:
		static Ref<Material> Create(const Ref<Shader>& shader, const std::string& name = "");
		static Ref<Material> Copy(const Ref<Material>& other, const std::string& name = "");
		virtual ~Material() = default;

		virtual void Invalidate() {}
		virtual void OnShaderReloaded() {}

		virtual void Set(const std::string& name, float value);
		virtual void Set(const std::string& name, int value);
		virtual void Set(const std::string& name, uint32_t value);
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
		virtual void Set(const std::string& name, const Ref<Texture2D>& texture, uint32_t arrayIndex);
		virtual void Set(const std::string& name, const Ref<TextureCube>& texture);

		virtual float& GetFloat(const std::string& name);
		virtual int32_t& GetInt(const std::string& name);
		virtual uint32_t& GetUInt(const std::string& name);
		virtual bool& GetBool(const std::string& name);
		virtual glm::vec2& GetVector2(const std::string& name);
		virtual glm::vec3& GetVector3(const std::string& name);
		virtual glm::vec4& GetVector4(const std::string& name);
		virtual glm::mat3& GetMatrix3(const std::string& name);
		virtual glm::mat4& GetMatrix4(const std::string& name);

		virtual Ref<Texture2D> GetTexture2D(const std::string& name);
		virtual Ref<TextureCube> GetTextureCube(const std::string& name);

		virtual Ref<Texture2D> TryGetTexture2D(const std::string& name);
		virtual Ref<TextureCube> TryGetTextureCube(const std::string& name);

		virtual uint32_t GetFlags() const { return m_MaterialFlags; }
		virtual void SetFlags(uint32_t flags) { m_MaterialFlags = flags; }

		virtual bool GetFlag(MaterialFlag flag) const { return (static_cast<uint32_t>(flag) & m_MaterialFlags) != 0; }
		virtual void SetFlag(MaterialFlag flag, bool value = true);

		virtual Ref<Shader> GetShader() { return m_Shader; }
		virtual const std::string& GetName() const { return m_Name; }

	protected:
		Material(const Ref<Shader>& shader, const std::string& name = "");

	protected:
		Ref<Shader> m_Shader;
		std::string m_Name;
		uint32_t m_MaterialFlags = static_cast<uint32_t>(MaterialFlag::DepthTest);

		// Material properties storage
		std::unordered_map<std::string, float> m_FloatUniforms;
		std::unordered_map<std::string, int> m_IntUniforms;
		std::unordered_map<std::string, uint32_t> m_UIntUniforms;
		std::unordered_map<std::string, bool> m_BoolUniforms;
		std::unordered_map<std::string, glm::vec2> m_Vec2Uniforms;
		std::unordered_map<std::string, glm::vec3> m_Vec3Uniforms;
		std::unordered_map<std::string, glm::vec4> m_Vec4Uniforms;
		std::unordered_map<std::string, glm::mat3> m_Mat3Uniforms;
		std::unordered_map<std::string, glm::mat4> m_Mat4Uniforms;
		std::unordered_map<std::string, Ref<Texture2D>> m_Texture2DUniforms;
		std::unordered_map<std::string, Ref<TextureCube>> m_TextureCubeUniforms;
	};

}
